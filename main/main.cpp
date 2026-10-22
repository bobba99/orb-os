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
#include "orb_platform.h"   // millis()
#include "display.h"
#include "display_idf.h"
#include "knob.h"
#include "sdcard.h"
#include "rtc_pcf85063.h"
#include "orb_prefs.h"
#include "orb_wifi.h"
#include "orb_net.h"
#include "flight.h"
#include "host.h"

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
#include "settings_view.h"
#include "chime_library.h"
#include "ui.h"

static const char *TAG = "orb";

void bringup_run();   // bringup.cpp

// The owner's timezone: NVS "tz" as Arduino stored it, else the compiled default.
static char s_tz[48] = TZ_STR;

static void psram_mark(const char *stage) {
    printf("[psram] %-26s free %6u KB\n", stage,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    printf("[intram] %-25s free %6u B  largest %5u B\n", stage,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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

// Flight Tracker's app-shell hooks, as src/app_hooks.cpp has them. Visibility is what
// src/main.cpp kept in g_radarViewActive: it decides how often the feed is polled.
static void flight_enter()          { ui_show_view(0); radar::knobEnter(); flight::setVisible(true); }
static void flight_exit()           { radar::knobExit(); flight::setVisible(false); }
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
        flight::loopTick();   // new traffic to the scope, drawn only while it is on screen

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
            // src/main.cpp's status tick: signal bars, "fresh" meaning a good poll in the
            // last 18 s, and the scope's own notice for whichever of WiFi, location or feed
            // is missing.
            const bool wifiUp = orb_wifi::connected();
            const bool feedFresh = wifiUp && flight::lastFeedOkMs() &&
                                   (millis() - flight::lastFeedOkMs() < 18000UL);
            ui_set_status(wifiUp, feedFresh, orb_wifi::rssi(), clk);
            flight::statusTick(wifiUp);
            // Settings > Location shows the centre; stored there whether or not it is built.
            settingsview::setHomeCoords(flight::homeLat(), flight::homeLon(), flight::locationSet());
            // Once NTP has a real fix, keep it in the RTC for the next offline boot.
            static bool rtcSynced = false;
            if (!rtcSynced && orb_wifi::timeSynced() && now > 1700000000L) {
                struct tm utc;
                gmtime_r(&now, &utc);
                if (rtc_write(&utc)) { rtcSynced = true; printf("[rtc] saved NTP time\n"); }
            }
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
    host::loadSettings();   // brightness, sound and units; the host_* functions are in host.cpp
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
    display::setBrightness((uint8_t)host::brightness());

    display_idf::lock();
    // On Arduino display::begin() builds the radar UI itself; here it is a separate step.
    ui_create();
    ui_splash_show();
    theme_font::begin();
    flight::applySettings();   // the scope's settings, now that the radar exists
    lv_obj_t *radarScreen = lv_scr_act();
    psram_mark("after display+radar");

    clockview::init();
    psram_mark("after clockview");
    // Slot order is app_shell::Slot's: Clock, Flight Tracker, Headlines, Settings.
    app_shell::add(clockview::screen(), theme_style::names().clock, nullptr, nullptr, false,
                   clockview::onEnter, clockview::onExit, !theme_style::apps().clock);
    app_shell::add(radarScreen, theme_style::names().flight, flight_press, flight_turn, false,
                   flight_enter, flight_exit, !theme_style::apps().flight);
    // Headlines is not ported (stage 7). Its slot is held by an empty screen the knob never
    // reaches, so APP_SETTINGS means Settings, as verifySlots() below checks.
    app_shell::add(lv_obj_create(nullptr), theme_style::names().headlines, nullptr, nullptr,
                   false, nullptr, nullptr, true);
    // Built the first time it is opened, as src/main.cpp registers it.
    app_shell::addLazy(theme_style::names().settings,
                       settingsview::build, settingsview::destroy,
                       settingsview::onPress, settingsview::onTurn,
                       true, settingsview::onEnter, settingsview::onExit, false);
    app_shell::verifySlots(nullptr, settingsview::build);
    knob::begin();
    app_shell::begin();
    psram_mark("after app_shell::begin");
    display_idf::unlock();

    // src/main.cpp: rtc_begin(), rtc_seed_clock(), clock_wind::begin().
    rtc_begin();
    rtc_seed_clock();
    clock_wind::begin();
    chime_library::begin();   // the chime list Settings > Sound shows; heard once audio is ported

    // WiFi on the network the Orb already knows, then the feed. src/main.cpp joins before
    // building the apps and blocks on it; here the clock is up first and the network follows.
    orb_wifi::begin();
    orb_net::begin();   // the one HTTPS client task; every request goes through it
    flight::begin();

    // src/main.cpp: a card notice, or WiFi setup, instead of the clock. Setup is owed when the
    // Orb knows no network at all, or after an Arduino Settings > Reset. "The Orb Setup" opens
    // as well in that case (orb_wifi), so the phone and the knob are both on offer.
    const bool wantWifiSetup = orb_wifi::savedCount() == 0 || orb_prefs::getBool("needsWifiSetup", false);
    if (!sdcard::mounted() || wantWifiSetup) {
        display_idf::lock();
        app_shell::selectApp(app_shell::APP_SETTINGS);
        app_shell::setCaptured(true);   // Settings captures the knob on entry; match that
        if (!sdcard::mounted()) settingsview::openNoSdCardNotice(wantWifiSetup);
        else                    settingsview::openWifiSetupPrompt();
        display_idf::unlock();
    }

    // Core 1 with LVGL, as Arduino's loop() ran; core 0 stays free for the network.
    xTaskCreatePinnedToCore(app_loop, "app", 8192, nullptr, 5, nullptr, 1);

    printf("setup done (firmware %s, ESP-IDF stage 4)\n", FW_VERSION);
    printf("not ported yet: Headlines, weather, photos,\n"
           "  audio (ES8311), IMU and battery, idle dimming, mDNS and the web server,\n"
           "  factory reset, baking a new theme\n");
}
