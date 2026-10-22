// The Orb OS — the Flight Tracker's host side on ESP-IDF.
//
// Ports what the Flight Tracker needs from the Arduino firmware's top level, each part named
// after where it came from:
//   settings      src/storage.cpp loadSettings(), applyThemeSettings(), applyStoredDisplaySettings()
//   network task  src/net/net_task.cpp, the aircraft-feed half (weather, headlines, routes and
//                 photos are not ported)
//   hand-off      src/main.cpp loop(), the snapshot swap and the catch-up render
//   query radius  src/app_hooks.cpp queryRadiusKm() and deadZoneKm()
// Settings changes two of these, the range (setRangeKm) and the centre (setHome, by way of
// location.cpp); the rest are read-only here.
#include "flight.h"

#include <map>
#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "orb_platform.h"
#include "config.h"
#include "aircraft.h"
#include "adsb_client.h"
#include "radar_view.h"
#include "ui.h"
#include "theme_style.h"
#include "display_idf.h"
#include "orb_prefs.h"
#include "orb_wifi.h"
#include "location.h"
#include "routes.h"

namespace {

RadarSettings s_settings = {HOME_LAT_DEFAULT, HOME_LON_DEFAULT, RANGE_KM_DEFAULT};
bool  s_locationSet = false;
float s_proximityKm = 0.0f;
int   s_deadZonePx = 0;

AdsbClient s_adsb;

// The table and the snapshot, as in src/shared_state.cpp. The table is upserted, never
// replaced, so one missed poll dims a contact instead of deleting it.
SemaphoreHandle_t                s_mutex = nullptr;
std::map<std::string, Aircraft>  s_table;
std::vector<Aircraft>            s_aircraft;   // core 0 writes, under s_mutex
std::vector<Aircraft>            s_snap;       // core 1 draws from this
volatile bool     s_dirty = false;
volatile bool     s_visible = false;
volatile uint32_t s_lastFeedOkMs = 0;
volatile bool     s_requery = false;   // range or centre changed: begin() again
volatile bool     s_dropTable = false; // the centre moved: the old contacts are somewhere else

float query_radius_km() {
    float km = s_settings.rangeKm * ADSB_QUERY_MULT;
    if (s_proximityKm > 0.0f && s_proximityKm * 1.2f > km) km = s_proximityKm * 1.2f;
    return km < ADSB_QUERY_MIN_KM ? ADSB_QUERY_MIN_KM : (km > ADSB_QUERY_MAX_KM ? ADSB_QUERY_MAX_KM : km);
}

float dead_zone_km() {
    if (s_deadZonePx <= 0) return 0.0f;
    const float px = (float)(s_deadZonePx < (int)RADAR_R_OUTER_PX ? s_deadZonePx : (int)RADAR_R_OUTER_PX);
    return (px / (float)RADAR_R_OUTER_PX) * s_settings.rangeKm;
}

// src/net/net_task.cpp applyPolledAircraft(), unchanged. Called under s_mutex.
void apply_polled(std::vector<Aircraft> &fresh, uint32_t nowMs) {
    for (Aircraft &ac : fresh) {
        ac.lastUpdateMs = nowMs;
        s_table[std::string(ac.hex.c_str())] = ac;
    }
    for (auto it = s_table.begin(); it != s_table.end(); ) {
        it = (nowMs - it->second.lastUpdateMs > AC_HARD_EXPIRE_MS) ? s_table.erase(it) : std::next(it);
    }
    s_aircraft.clear();
    s_aircraft.reserve(s_table.size());
    for (auto &kv : s_table) s_aircraft.push_back(kv.second);
}

// radar::setFeedNote writes an LVGL label. The Arduino network task calls it from core 0
// without any lock, which races the renderer; here it takes the display lock for the call.
void feed_note(const char *note) {
    display_idf::lock();
    radar::setFeedNote(note);
    display_idf::unlock();
}

// The aircraft-feed half of src/net/net_task.cpp adsb_task(), with its polling rules intact:
// full rate while the tracker is on screen, once a minute for ten minutes after (and after
// boot), then nothing; no polling at all until a location is set; exponential backoff on
// failures and minutes-long backoff on refusals; never a restart over a feed outage.
void net_task(void *) {
    std::vector<Aircraft> fresh;
    uint32_t lastPoll = 0, backoffMs = 0;
    uint32_t lastOnScreenMs = 0;
    bool wasOnScreen = false;
    int failCount = 0;
    for (;;) {
        if (s_requery) {   // src/net/net_task.cpp, the g_requery step
            s_requery = false;
            s_adsb.begin(s_settings.homeLat, s_settings.homeLon, query_radius_km());
            lastPoll = 0;   // poll at once at the new radius
            if (s_dropTable && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                s_dropTable = false;
                s_table.clear();
                s_aircraft.clear();
                s_dirty = true;
                xSemaphoreGive(s_mutex);
            }
        }
        const bool conn = orb_wifi::connected();
        // src/main.cpp host_locate_if_unset(): one lookup per boot, once there is a network.
        static bool locateTried = false;
        if (conn && !locateTried) {
            locateTried = true;
            location::locateIfUnset();
        }
        // The radar asked for a route (an aircraft was selected): one lookup, here, between
        // polls, as src/net/net_task.cpp does it.
        if (conn) routes::service();
        const bool radarActive = theme_style::apps().flight;
        if (conn && radarActive) {
            const uint32_t nowMs = millis();
            const bool onScreen = s_visible;
            if (onScreen) lastOnScreenMs = nowMs;
            if (onScreen && !wasOnScreen) lastPoll = 0;   // just opened: ask now
            wasOnScreen = onScreen;
            const bool warm = onScreen || (nowMs - lastOnScreenMs) < 600000UL;
            const uint32_t interval = (onScreen ? POLL_INTERVAL_MS : 60000UL) + backoffMs;
            if (s_locationSet && warm && (lastPoll == 0 || nowMs - lastPoll >= interval)) {
                lastPoll = nowMs;
                if (s_adsb.poll(fresh)) {
                    printf("[adsb] fetched %u aircraft\n", (unsigned)fresh.size());
                    failCount = 0;
                    backoffMs = 0;
                    feed_note(nullptr);
                    const uint32_t receivedMs = millis();
                    s_lastFeedOkMs = receivedMs;
                    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                        apply_polled(fresh, receivedMs);
                        s_dirty = true;
                        xSemaphoreGive(s_mutex);
                    }
                } else if (s_adsb.lastWasRefused()) {
                    ++failCount;
                    backoffMs = backoffMs < 60000UL ? 60000UL : (backoffMs < 300000UL ? backoffMs * 2 : 300000UL);
                    feed_note("No aircraft feed\nThe service is refusing requests\nTrying again in a few minutes");
                    printf("[adsb] refused by the feed (HTTP %d); waiting ~%lus\n",
                           s_adsb.refusedStatus(), (unsigned long)(backoffMs / 1000));
                } else {
                    ++failCount;
                    backoffMs = backoffMs == 0 ? 2000UL : (backoffMs < 15000UL ? backoffMs * 2 : 30000UL);
                    printf("[adsb] poll failed (backing off ~%lus)\n", (unsigned long)(backoffMs / 1000));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

}  // namespace

void flight::applySettings() {
    // src/storage.cpp loadSettings(): the owner's values, under the Arduino keys and types.
    s_settings.homeLat = orb_prefs::getDouble("homeLat", HOME_LAT_DEFAULT);
    s_settings.homeLon = orb_prefs::getDouble("homeLon", HOME_LON_DEFAULT);
    // An Orb that stored a position before locSet existed has one; see loadSettings().
    s_locationSet      = orb_prefs::getBool("locSet", orb_prefs::isKey("homeLat"));
    s_settings.rangeKm = orb_prefs::getFloat("rangeKm", RANGE_KM_DEFAULT);
    s_proximityKm      = orb_prefs::getFloat("proxkm", 0.0f);
    int maxAc          = (int)orb_prefs::getInt("maxac", 12);
    const int trailLen = (int)orb_prefs::getInt("traillen", 2);
    const int units    = (int)orb_prefs::getInt("units", 0);
    bool  hideGround   = false;
    int   minAltFt     = 0;

    // applyThemeSettings(): -1 (or 0 for range) means the theme has no opinion.
    const theme_style::Radar &rs = theme_style::radar();
    if (rs.rangeKm > 0.0f)  s_settings.rangeKm = rs.rangeKm;
    if (rs.maxAircraft > 0) maxAc = rs.maxAircraft;
    if (rs.minAltFt >= 0)   minAltFt = rs.minAltFt;
    if (rs.hideGround >= 0) hideGround = rs.hideGround != 0;
    if (rs.deadZonePx >= 0) s_deadZonePx = rs.deadZonePx > (int)RADAR_R_OUTER_PX ? (int)RADAR_R_OUTER_PX
                                                                                : rs.deadZonePx;
    if (maxAc > ADSB_MAX_AIRCRAFT) maxAc = ADSB_MAX_AIRCRAFT;
    if (maxAc < 1) maxAc = 1;

    // applyStoredDisplaySettings(), read-only: the radar skin's one-time renumbering is
    // computed here but not written back, which the Arduino firmware does on its next boot.
    int t = (int)orb_prefs::getInt("theme", 4);
    if (!orb_prefs::getBool("themeMigV2", false))
        t = (t == 1) ? THEME_ORB : (t == 3) ? THEME_MILITARY : THEME_AVIATOR;
    const bool sweep    = orb_prefs::getBool("sweep", true);
    const bool airports = orb_prefs::getBool("airports", true);
    if (rs.hideGround < 0) hideGround = orb_prefs::getBool("hideground", false);
    if (rs.minAltFt < 0)   minAltFt   = (int)orb_prefs::getInt("minalt", 0);
    const bool milOnly  = orb_prefs::getBool("milonly", false);

    radar::setTheme(t);
    ui_apply_theme(t);
    radar::setSweepEnabled(sweep);
    radar::setAirportsEnabled(airports);
    radar::setTrailLength(trailLen);
    radar::setMaxOnScreen(maxAc);
    ui_set_units(units);

    s_adsb.setHideGround(hideGround);
    s_adsb.setMinAltFt((float)minAltFt);
    s_adsb.setMinDistKm(dead_zone_km());
    s_adsb.setMilitaryOnly(milOnly);

    printf("[flight] home %.4f,%.4f (%s), range %.0f km, max %d aircraft, minAlt %d ft, hideGround %d\n",
           s_settings.homeLat, s_settings.homeLon, s_locationSet ? "set" : "NOT set",
           (double)s_settings.rangeKm, maxAc, minAltFt, (int)hideGround);
}

void flight::begin() {
    s_mutex = xSemaphoreCreateMutex();
    s_adsb.begin(s_settings.homeLat, s_settings.homeLon, query_radius_km());
    // TLS and the parse run on orb_net's client task now; this one waits for them.
    xTaskCreatePinnedToCore(net_task, "net", 8192, nullptr, 1, nullptr, 0);
}

void flight::setVisible(bool on) { s_visible = on; }

double flight::homeLat() { return s_settings.homeLat; }
double flight::homeLon() { return s_settings.homeLon; }
float  flight::rangeKm() { return s_settings.rangeKm; }
bool   flight::locationSet() { return s_locationSet; }

void flight::setHome(double lat, double lon) {
    s_settings.homeLat = lat;
    s_settings.homeLon = lon;
    s_locationSet = true;
    s_dropTable = true;
    s_requery = true;
}

void flight::setRangeKm(float km) {
    s_settings.rangeKm = km;
    orb_prefs::putFloat("rangeKm", km);
    s_adsb.setMinDistKm(dead_zone_km());   // a zone in pixels is a new distance at a new range
    s_requery = true;
    radar::update(s_snap, s_settings);     // the zoom shows now, from the last snapshot
    ui_on_data_updated();
}

void flight::loopTick() {
    // src/main.cpp loop(): swap under the lock, render outside it, and only while visible.
    if (s_dirty && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        s_snap.swap(s_aircraft);
        s_dirty = false;
        xSemaphoreGive(s_mutex);
        if (s_visible) {
            radar::update(s_snap, s_settings);
            ui_on_data_updated();
        }
    }
    // The catch-up frame: traffic that arrived while another app was on screen is drawn the
    // moment the tracker comes back, rather than at the next poll.
    static bool wasVisible = false;
    if (s_visible && !wasVisible) {
        radar::update(s_snap, s_settings);
        ui_on_data_updated();
    }
    wasVisible = s_visible;
}

void flight::statusTick(bool wifiUp) {
    const uint32_t ok = s_lastFeedOkMs;
    radar::setFeedStatus(wifiUp, (uint32_t)((millis() - ok) / 1000UL), s_locationSet);
}

uint32_t flight::lastFeedOkMs() { return s_lastFeedOkMs; }
