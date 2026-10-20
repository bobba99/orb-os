// The Orb OS — the knob, on ESP-IDF.
//
// Ported from the Arduino firmware's src/knob.cpp. The detent, rock and switch logic is that
// file's, with its reasoning kept beside it; only where the edges come from has changed.
// Arduino decoded quadrature in a GPIO interrupt on every edge of A and B. Here the PCNT
// peripheral counts the edges in hardware and interrupts once per detent, which is what
// REWRITE_PLAN.md stage 2 asks for: no interrupt storm while the knob spins, and a glitch
// filter in front of the count.
#include "knob.h"
#include "board.h"

#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "knob";

// Most KY-040 encoders emit 4 quadrature edges per physical click ("detent").
// If turns feel doubled or halved on real hardware, tune this to 2 or 1.
static constexpr int KNOB_STEPS_PER_DETENT = 4;

// Edges shorter than this are contact chatter, not rotation. The Arduino decoder had no
// filter and relied on the quadrature table rejecting impossible transitions; PCNT can
// throw the chatter away before it is ever counted. 1 us is far below the fastest real
// edge spacing of a hand-turned KY-040 (milliseconds).
static constexpr int KNOB_GLITCH_NS = 1000;

// How many detents the leftward half of a rock may span before it stops being a rock.
// Two, so a slightly heavy flick still counts, but a deliberate scroll does not.
static constexpr int ROCK_MAX_RUN = 2;

// A rock has to be humanly possible, and QUICK. Below the minimum it is contact bounce.
// The reversal has to come within ROCK_QUICK_MS of the last detent the other way: a flick
// back and forth is one motion, and a hand reverses inside a quarter of a second when it
// means the gesture. It was 800 ms, and at 800 ms the ordinary back-and-forth of browsing a
// list (down two, up one to reread) was a rock; Zion, in the News app: "it's very easy to
// accidentally go into the main menu when you're just scrolling back and forth." The pause
// that ends a run stays wider, because that is a different question: how long after a turn
// the next detent is a new gesture rather than the same one.
static constexpr uint32_t ROCK_MIN_GAP_MS = 45;
static constexpr uint32_t ROCK_QUICK_MS   = 250;
static constexpr uint32_t ROCK_MAX_GAP_MS = 900;

static constexpr uint32_t SW_DEBOUNCE_MS = 200;  // min time between accepted presses. Wide on purpose:
                                                  //   this switch bounces heavily, and 200ms is still far
                                                  //   faster than anyone deliberately selects menu items,
                                                  //   so one physical click can't become two/three.
static constexpr uint32_t LONG_PRESS_MS  = 8000;   // hold to force a recovery reboot (long enough
                                                    // that incidental contact while touching the
                                                    // screen next to the knob can't trigger it)

// Milliseconds since boot. esp_timer_get_time() is safe from an interrupt, which is where
// most of the callers below run; it stands in for Arduino's millis() throughout.
static inline uint32_t IRAM_ATTR now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

static pcnt_unit_handle_t s_unit = nullptr;

// Pending input for the app shell to consume (written + read on the loop thread).
static int32_t  s_pendingDelta = 0;
static int      s_lastDir      = 0;    // -1 left, +1 right, 0 = nothing turned yet
static uint32_t s_lastDirMs    = 0;

// The Rock, detected IN THE ISR.
//
// It used to be worked out in poll(), from the net change in position since the last call.
// That threw whole rocks away. poll() sees one number, and a left detent followed by a right
// one between two calls nets to zero: the position is back where it started, `detent !=
// s_lastDetent` is false, and the rock never happened as far as anything downstream knows.
// The interrupt had caught both detents perfectly; poll() collapsed them.
//
// Which made it a frame-rate bug wearing an input bug's clothes, and explains the report
// that it worked from every app except the Clock. The Clock rotates a 539 KB minute hand and
// a 294 KB hour hand with shadows every frame, so its loop iteration is the longest on the
// device and poll() runs least often there. Widest window, most rocks swallowed.
//
// Here every detent is seen as it happens, with its own timestamp, and nothing can cancel
// out. On ESP-IDF "the ISR" is PCNT's watch-point callback, one call per committed detent.
static volatile int      s_isrLastDir   = 0;
static volatile uint32_t s_isrLastDirMs = 0;
// How many detents the current direction has run for. A rock is a small deliberate wiggle,
// so the turn INTO it has to be small: without this, five detents left followed by one
// right counted as a rock, which is just ordinary browsing and is why the menu appeared to
// open on any turn at all.
static volatile int      s_isrRunLen    = 0;
static volatile int32_t  s_detent       = 0;   // committed detents; the one true stream
static volatile uint32_t s_rockMs       = 0;   // the detent that completed a reversal, either way
static volatile int32_t  s_rockDetent   = 0;   // s_detent at that moment, for the settle check
static volatile uint32_t s_rockGapMs    = 0;
static volatile bool s_pendingPress = false;
static volatile bool s_pendingLong  = false;

// Button edge + debounce lives in the ISR (like rotation), so a quick tap is caught
// the instant it happens even if the main loop is busy (e.g. Spy Cam decoding a
// frame). Polling the level once per loop can miss a press-and-release that both
// happen inside one slow loop iteration; a hardware interrupt cannot.
static volatile uint32_t s_swLastEdgeMs   = 0;
static volatile int      s_swLevel        = 1;
static volatile uint32_t s_swPressStartMs = 0;
static volatile bool     s_swLongFired    = false;

// ONE detent stream, committed with hysteresis, feeding everything.
//
// This is the fix for a menu that opened by itself. The detent used to be derived as
//
//     const int32_t det = s_rawPos / KNOB_STEPS_PER_DETENT;
//
// which has no hysteresis. A knob resting ON a boundary needs only one step of mechanical
// or electrical dither to flip that value back and forth, 8 -> 7 -> 8, and each flip looked
// like a change of direction a couple of milliseconds apart with a run length of one: the
// exact shape of a rock. So stopping mid-turn opened the app menu, which is what "I turned
// it three times and it went to the menu" was.
//
// So: a detent is committed only after a FULL detent of travel from the last committed
// position, in either direction. Dither of one to three steps commits nothing, ever.
//
// On PCNT that rule is the counter's own limits. The unit counts between -4 and +4 and
// resets to zero on reaching either, so its count is always "steps since the last committed
// detent", exactly what the Arduino code tracked as s_rawPos - s_anchor, and reaching a limit
// is exactly its commit condition. Nothing in this function changed in the port.
static void IRAM_ATTR on_detent(int dir) {
    s_detent += dir;
    const uint32_t now = now_ms();
    // A rock is a SHORT turn one way, then straight back, as one gesture.
    //   run length : the turn into the reversal was a flick, not a scroll
    //   gap        : one gesture rather than two decisions
    //   minimum gap: a hand cannot reverse in five milliseconds. Anything faster is the
    //                encoder, not the person, and treating it as input is what let a
    //                resting knob open the menu.
    //
    // EITHER direction (UX-011): the owner reaches for the menu without thinking about which
    // way his hand goes first. The run-length rule is what actually stops ordinary browsing
    // from qualifying, and input_router does not test for a rock at all while the menu is
    // already open, so a false positive can only ever happen inside an app.
    //
    // A PAUSE ENDS A RUN. A detent that arrives after longer than the rock window is the
    // start of something new, whichever way it goes. Without this, browsing the menu to the
    // right and then rocking right-first counted the rock's opening detent as the sixth of
    // the run and refused it (CanadianAvenger, 2026-09-13).
    const bool fresh = (now - s_isrLastDirMs) > ROCK_MAX_GAP_MS;
    if (!fresh && s_isrLastDir != 0 && dir != s_isrLastDir && s_isrRunLen <= ROCK_MAX_RUN) {
        const uint32_t gap = now - s_isrLastDirMs;
        if (gap >= ROCK_MIN_GAP_MS && gap <= ROCK_QUICK_MS) {
            s_rockGapMs  = gap;
            s_rockDetent = s_detent;
            s_rockMs     = now ? now : 1;   // never 0, which means "never happened"
        }
    }
    s_isrRunLen    = (!fresh && dir == s_isrLastDir) ? s_isrRunLen + 1 : 1;
    s_isrLastDir   = dir;
    s_isrLastDirMs = now;
}

// PCNT reached +4 or -4: one full detent of travel. The unit has already reset to zero.
static bool IRAM_ATTR on_pcnt_reach(pcnt_unit_handle_t, const pcnt_watch_event_data_t *ev, void *) {
    on_detent(ev->watch_point_value > 0 ? +1 : -1);
    return false;   // no task was woken
}

// Debounced in the ISR. A RELEASE (rising edge) is always accepted so s_swLevel can
// never get "stuck" LOW (an earlier bug counted a phantom hold toward the long-press
// reboot) — but it MUST also stamp s_swLastEdgeMs. That's the key: without stamping
// the release, the switch's release bounce (open/closed/open) produced falling edges
// that landed >SW_DEBOUNCE_MS after the original press and each registered as a NEW
// press, so one physical click typed 2-3 characters. A PRESS (falling edge) is only
// accepted once SW_DEBOUNCE_MS has passed since ANY edge (press or release), which
// rejects both press bounce and the tail of a just-released button.
static void IRAM_ATTR knob_sw_isr(void *) {
    const uint32_t now = now_ms();
    const int lvl = gpio_get_level(BOARD_KNOB_SW);
    if (lvl) {
        s_swLevel = 1;              // release: trust instantly, never gets "stuck"...
        s_swLastEdgeMs = now;       // ...but timestamp it so the release bounce is debounced too
        return;
    }
    if (now - s_swLastEdgeMs < SW_DEBOUNCE_MS) return;   // reject bounce (press- and release-side)
    s_swLastEdgeMs = now;
    s_swLevel = 0;
    s_pendingPress    = true;
    s_swPressStartMs  = now;
    s_swLongFired     = false;
}

void knob::begin() {
    pcnt_unit_config_t unitCfg = {};
    unitCfg.low_limit  = -KNOB_STEPS_PER_DETENT;
    unitCfg.high_limit =  KNOB_STEPS_PER_DETENT;
    ESP_ERROR_CHECK(pcnt_new_unit(&unitCfg, &s_unit));

    pcnt_glitch_filter_config_t filter = {};
    filter.max_glitch_ns = KNOB_GLITCH_NS;
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_unit, &filter));

    // Full quadrature, all four edges per cycle, as the Arduino decode table counted them.
    // Each channel counts the edges of one line, with the other line deciding the direction.
    // The signs match that table: A rising while B is high counts down, B rising while A is
    // high counts up, so clockwise is positive as before.
    pcnt_chan_config_t chACfg = {};
    chACfg.edge_gpio_num  = BOARD_KNOB_A;
    chACfg.level_gpio_num = BOARD_KNOB_B;
    pcnt_channel_handle_t chA = nullptr;
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &chACfg, &chA));
    pcnt_chan_config_t chBCfg = {};
    chBCfg.edge_gpio_num  = BOARD_KNOB_B;
    chBCfg.level_gpio_num = BOARD_KNOB_A;
    pcnt_channel_handle_t chB = nullptr;
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &chBCfg, &chB));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chA, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                                      PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chA, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                       PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chB, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                      PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chB, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                       PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    // Pull-ups, as INPUT_PULLUP gave on Arduino. Some KY-040 boards carry their own and
    // some do not; with neither, the lines float and the count wanders on its own.
    gpio_pullup_en(BOARD_KNOB_A);
    gpio_pullup_en(BOARD_KNOB_B);

    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_unit,  KNOB_STEPS_PER_DETENT));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_unit, -KNOB_STEPS_PER_DETENT));
    pcnt_event_callbacks_t cbs = {};
    cbs.on_reach = on_pcnt_reach;
    ESP_ERROR_CHECK(pcnt_unit_register_event_callbacks(s_unit, &cbs, nullptr));
    ESP_ERROR_CHECK(pcnt_unit_enable(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_unit));

    gpio_config_t sw = {};
    sw.pin_bit_mask = 1ULL << BOARD_KNOB_SW;
    sw.mode         = GPIO_MODE_INPUT;
    sw.pull_up_en   = GPIO_PULLUP_ENABLE;
    sw.intr_type    = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&sw));
    // Already installed is fine: another component may have got there first.
    const esp_err_t isr = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(isr);
    s_swLevel = gpio_get_level(BOARD_KNOB_SW);
    ESP_ERROR_CHECK(gpio_isr_handler_add(BOARD_KNOB_SW, knob_sw_isr, nullptr));

    ESP_LOGI(TAG, "ready on GPIO%d(A)/%d(B)/%d(SW), PCNT quadrature — turn + push",
             BOARD_KNOB_A, BOARD_KNOB_B, BOARD_KNOB_SW);
}

void knob::poll() {
    // --- Rotation -------------------------------------------------------------------
    // The SAME committed stream the ISR builds, not a second derivation. Two derivations
    // meant two answers about what the knob had done, and the one that decided rocks had no
    // hysteresis.
    static int32_t s_lastDetent = 0;
    const int32_t detent = s_detent;
    if (detent != s_lastDetent) {
        int32_t delta = detent - s_lastDetent;
        s_lastDetent = detent;
        s_pendingDelta += delta;
        // The rock is NOT decided here; the ISR owns it. This is only the log, kept at poll
        // resolution so the two can be compared: a reversal the ISR recorded that never
        // appears here is a rock this code used to lose entirely.
        const int dir = delta > 0 ? 1 : -1;
        const uint32_t now = now_ms();
        // EVERY reversal, with the gap that decides whether it counts as a Rock, so the
        // threshold can be set from what this owner's hand and this knob actually do.
        if (s_lastDir != 0 && dir != s_lastDir) {
            const uint32_t gap = now - s_lastDirMs;
            ESP_LOGI(TAG, "REVERSAL %s->%s gap=%lums%s",
                     s_lastDir > 0 ? "R" : "L", dir > 0 ? "R" : "L", (unsigned long)gap,
                     (gap >= ROCK_MIN_GAP_MS && gap <= ROCK_MAX_GAP_MS)
                         ? "  (in the rock window; run length decides)" : "");
        }
        s_lastDir   = dir;
        s_lastDirMs = now;
        ESP_LOGD(TAG, "turned %s  (pos=%ld)", delta > 0 ? "RIGHT (CW)" : "LEFT (CCW)", (long)detent);
    }

    // --- Button: the press is caught in the ISR (knob_sw_isr); here we only watch for a
    // sustained hold, which by definition spans many poll() calls.
    if (s_swLevel == 0 && !s_swLongFired && (now_ms() - s_swPressStartMs) >= LONG_PRESS_MS) {
        s_swLongFired = true;
        s_pendingLong = true;
        ESP_LOGI(TAG, "long-press (reboot)");
    }
}

int32_t knob::takeDelta() {
    int32_t d = s_pendingDelta;
    s_pendingDelta = 0;
    return d;
}

// Committed detents in steps, plus the steps counted since the last one. The two reads are
// not atomic together, so a detent landing between them can make one call read a detent
// short; callers compare snapshots to ask "did it move at all", which that cannot fool.
int32_t knob::rawPosition() {
    int count = 0;
    if (s_unit) pcnt_unit_get_count(s_unit, &count);
    return s_detent * KNOB_STEPS_PER_DETENT + count;
}

uint32_t knob::lastRockMs()     { return s_rockMs; }
uint32_t knob::lastRockGapMs()  { return s_rockGapMs; }
int32_t  knob::lastRockDetent() { return s_rockDetent; }
int32_t  knob::detentCount()    { return s_detent; }

bool knob::takePress() {
    bool p = s_pendingPress;
    s_pendingPress = false;
    return p;
}

bool knob::pendingPress() { return s_pendingPress; }

bool knob::takeLongPress() {
    bool p = s_pendingLong;
    s_pendingLong = false;
    return p;
}

uint32_t knob::heldMs() {
    return (s_swLevel == 0) ? (now_ms() - s_swPressStartMs) : 0;
}

uint32_t knob::longPressMs() { return LONG_PRESS_MS; }
