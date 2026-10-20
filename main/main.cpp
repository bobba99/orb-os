// The Orb OS — ESP-IDF entry point.
//
// Stage 2 of REWRITE_PLAN.md: the clock and the flight tracker, running the very UI code the
// Arduino firmware and the simulator run (components/orb_ui compiles it from src/). The boot
// order is the Arduino firmware's setup() (src/main.cpp), trimmed to what these two apps
// need; each step names the Arduino step it stands for. What is not here yet is listed at the
// bottom of the boot, so it is said once rather than discovered.
//
// LVGL runs in esp_lvgl_port's task. The app loop below holds the display lock for its whole
// pass and lets go only while it sleeps, which is when that task renders, so every LVGL call
// the shared code makes is serialised with the renderer exactly as it was in Arduino's loop().
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "lvgl.h"

#include "config.h"
#include "display.h"
#include "display_idf.h"
#include "knob.h"
#include "sdcard.h"
#include "rtc_pcf85063.h"
#include "orb_prefs.h"

#include "app_shell.h"
#include "app_theme.h"
#include "input_router.h"
#include "theme_select.h"
#include "theme_style.h"
#include "theme_art.h"
#include "theme_font.h"
#include "theme_audio.h"
#include "clock_view.h"
#include "clock_wind.h"
#include "wind_notice.h"
#include "radar_view.h"
#include "ui.h"

static const char *TAG = "orb";

void bringup_run();   // bringup.cpp

// The owner's timezone: NVS "tz" as Arduino stored it, else the compiled default.
static char s_tz[48] = TZ_STR;

// The idle / face-down / update-in-progress combination lives in src/main.cpp's
// applyBrightness() and is not ported; for now the owner's level is simply applied.
static int s_brightness = BRIGHTNESS_DEFAULT;

static void psram_mark(const char *stage) {
    printf("[psram] %-26s free %6u KB\n", stage,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    printf("[intram] %-25s free %6u B  largest %5u B\n", stage,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// --- host functions the shared code calls (src/ declares them extern) ----------------

// update_ui's hook: an update notice on screen puts the panel at full brightness, however
// dim the owner keeps it. As src/main.cpp host_update_bright(), minus the idle state that
// is not ported.
void host_update_bright(bool on) {
    display::noteActivity();
    display::setBrightness(on ? 255 : (uint8_t)s_brightness);
}

// The place's name for the flight tracker's location line: the same NVS key
// src/location.cpp saves it under. False when nothing has ever named it.
bool host_location_name(char *out, size_t n) {
    if (!n) return false;
    return orb_prefs::getString("homeName", out, n) && out[0];
}

// src/main.cpp utc_to_time() + rtc_seed_clock(): the RTC holds UTC, mktime() assumes local
// time, so convert under UTC0 and put the owner's zone back afterwards.
static void rtc_seed_clock() {
    struct tm utc;
    if (!rtc_read(&utc)) { printf("[rtc] no valid time stored\n"); return; }
    setenv("TZ", "UTC0", 1); tzset();
    const time_t t = mktime(&utc);
    setenv("TZ", s_tz, 1); tzset();
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    printf("[rtc] system clock seeded from RTC\n");
}

// src/storage.cpp applyThemeSettings(), the clock half: the mainspring and the theme's sounds
// belong to the design and are replaced whole whenever one is applied.
static void apply_theme_clock() {
    const theme_style::Clock &cs = theme_style::clock();
    clock_wind::applyTheme(cs.windOn, cs.windSecs, cs.windTurns, cs.windSound, cs.windNotice);
    theme_audio::load();
}

// Flight Tracker's app-shell hooks, as src/app_hooks.cpp has them minus the network-task
// gating (g_radarViewActive), which arrives with the feed.
static void flight_enter()          { ui_show_view(0); radar::knobEnter(); }
static void flight_exit()           { radar::knobExit(); }
static void flight_turn(int delta)  { radar::knobTurn(delta); }
static void flight_press()          { radar::knobPress(); }

// src/main.cpp loop(), the parts these two apps use.
static void app_loop(void *) {
    for (;;) {
        display_idf::lock();
        knob::poll();
        wind_notice::tick();
        wind_notice::animate();
        if (knob::takeLongPress()) {   // held ~8 s: the manual recovery reboot
            printf("[main] knob long-press -> reboot\n");
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_restart();
        }
        const int32_t kd = knob::takeDelta();
        const bool pressed = knob::takePress();
        if (kd != 0 || pressed) display::noteActivity();
        input_router::dispatch((int)kd, pressed);
        input_router::tick();

        // The HUD date and the scope's clock, every 5 s, as the Arduino status tick does.
        static TickType_t lastStatus = 0;
        if (xTaskGetTickCount() - lastStatus > pdMS_TO_TICKS(5000)) {
            lastStatus = xTaskGetTickCount();
            char clk[8] = "--:--";
            time_t now = time(nullptr);
            struct tm ti;
            if (now > 1700000000L && localtime_r(&now, &ti)) {
                snprintf(clk, sizeof(clk), "%02d:%02d", ti.tm_hour, ti.tm_min);
                char date[20];
                strftime(date, sizeof(date), "%d %b %Y", &ti);
                ui_set_date(date);
            }
            ui_set_status(false, false, -127, clk);   // no WiFi yet: the HUD says so
        }
        display_idf::unlock();
        vTaskDelay(pdMS_TO_TICKS(5));   // the Arduino loop's own 5 ms pace
    }
}

extern "C" void app_main(void) {
#if CONFIG_ORB_BRINGUP_PAGE
    bringup_run();
    return;
#endif
    printf("\nThe Orb OS boot (ESP-IDF)\n");
    psram_mark("boot start");

    // Settings first: everything below reads them. Never erases (orb_prefs.h).
    orb_prefs::begin();
    orb_prefs::getString("tz", s_tz, sizeof(s_tz), TZ_STR);
    s_brightness = (int)orb_prefs::getInt("bright", BRIGHTNESS_DEFAULT);
    setenv("TZ", s_tz, 1); tzset();

    sdcard::begin();
    psram_mark("after sdcard");

    // src/main.cpp: loadSettings(), app_theme::init(), theme_select::init(),
    // applyThemeSettings(), theme_art::begin().
    app_theme::init();
    theme_select::init();
    apply_theme_clock();
    theme_art::begin();
    printf("[theme] wearing '%s'\n", theme_select::activeSlug()[0] ? theme_select::activeSlug() : "(stock)");
    psram_mark("after theme");

    if (!display::begin()) ESP_LOGE(TAG, "display did not come up");
    display::setBrightness((uint8_t)s_brightness);

    display_idf::lock();
    // On Arduino display::begin() builds the radar UI itself; here it is a separate step.
    ui_create();
    ui_splash_show();
    theme_font::begin();
    lv_obj_t *radarScreen = lv_scr_act();
    psram_mark("after display+radar");

    clockview::init();
    psram_mark("after clockview");
    // Slot order is app_shell::Slot's: Clock, then Flight Tracker. Headlines and Settings
    // come after them and are not ported yet, so nothing registers in their places.
    app_shell::add(clockview::screen(), theme_style::names().clock, nullptr, nullptr, false,
                   clockview::onEnter, clockview::onExit, !theme_style::apps().clock);
    app_shell::add(radarScreen, theme_style::names().flight, flight_press, flight_turn, false,
                   flight_enter, flight_exit, !theme_style::apps().flight);
    knob::begin();
    app_shell::begin();
    psram_mark("after app_shell::begin");
    display_idf::unlock();

    // src/main.cpp: rtc_begin(), rtc_seed_clock(), clock_wind::begin().
    rtc_begin();
    rtc_seed_clock();
    clock_wind::begin();

    // Core 1 with LVGL, as Arduino's loop() ran; core 0 stays free for the network.
    xTaskCreatePinnedToCore(app_loop, "app", 8192, nullptr, 5, nullptr, 1);

    printf("setup done (firmware %s, ESP-IDF stage 2)\n", FW_VERSION);
    printf("not ported yet: WiFi and the aircraft feed, NTP, Headlines, Settings, audio (ES8311),\n"
           "  IMU and battery, baked theme art from flash, idle dimming, the web server\n");
}
