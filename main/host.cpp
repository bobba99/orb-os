// The Orb OS — the host functions the shared UI code calls, on ESP-IDF.
//
// src/ declares these extern and each build supplies them: src/storage.cpp, src/location.cpp,
// src/app_hooks.cpp and src/net/wifi.cpp on Arduino, src/sim_main.cpp in the simulator, and
// this file here. Most are Settings' (settings_view.cpp). Each one does what its Arduino
// namesake does, under the same NVS keys and types, except where a later stage of
// REWRITE_PLAN.md owns the work; those say so and are honest no-ops rather than pretences.
//
// WiFi is the part that differs in kind. Arduino stashed the saved network, tried the new
// one, wrote it on success and rebooted. orb_wifi::tryNetwork() does all of that itself and
// keeps the owner's networks safe by construction (orb_wifi.h, promise 2), so commit,
// restore and reboot have nothing left to do here.
#include "host.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "display.h"
#include "orb_prefs.h"
#include "orb_wifi.h"
#include "audio.h"
#include "chime_library.h"
#include "ui.h"
#include "flight.h"

namespace {

int      s_brightness = BRIGHTNESS_DEFAULT;
uint32_t s_idleDimMs  = IDLE_DIM_MS;
int      s_volume     = 60;
int      s_tickVol    = 20;
int      s_chimeVol   = 30;
bool     s_soundRadar = false;
bool     s_soundChime = false;
int      s_wxUnits    = 0;

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// src/app_hooks.cpp is_imperial_region(), copied: that file is Arduino-only.
bool is_imperial_region(double lat, double lon) {
    if (lat >= 24.0 && lat <= 49.5 && lon >= -125.0 && lon <= -66.0)  return true;   // CONUS
    if (lat >= 51.0 && lat <= 72.0 && lon >= -170.0 && lon <= -129.0) return true;   // Alaska
    if (lat >= 18.5 && lat <= 22.5 && lon >= -161.0 && lon <= -154.0) return true;   // Hawaii
    if (lat >= 4.0  && lat <= 8.6  && lon >= -11.6  && lon <= -7.3)   return true;   // Liberia
    if (lat >= 9.5  && lat <= 28.6 && lon >= 92.0   && lon <= 101.2)  return true;   // Myanmar
    return false;
}

}  // namespace

// src/storage.cpp loadSettings(), the keys these functions own.
void host::loadSettings() {
    s_brightness = clamp((int)orb_prefs::getInt("bright", BRIGHTNESS_DEFAULT), 8, 255);
    s_idleDimMs  = orb_prefs::getUInt("idledim", IDLE_DIM_MS);
    s_volume     = (int)orb_prefs::getInt("vol", 60);
    s_tickVol    = (int)orb_prefs::getInt("tickVol", 20);
    s_chimeVol   = (int)orb_prefs::getInt("chimeVol", 30);
    s_soundRadar = orb_prefs::getBool("sndRadar", false);
    s_soundChime = orb_prefs::getBool("sndChime", false);
    s_wxUnits    = (int)orb_prefs::getInt("wxUnits", 0);
    audio_set_volume(s_volume);
    audio_set_tick_level(s_tickVol);
    audio_set_chime_level(s_chimeVol);
}

int host::brightness() { return s_brightness; }

// --- display --------------------------------------------------------------------------

int host_get_brightness() { return s_brightness; }
void host_set_brightness(int v, bool save) {
    s_brightness = clamp(v, 8, 255);
    display::setBrightness((uint8_t)s_brightness);
    if (save) orb_prefs::putInt("bright", s_brightness);
}

// update_ui's hook: an update notice on screen puts the panel at full brightness, however
// dim the owner keeps it. As src/main.cpp host_update_bright(), minus the idle state that
// is not ported.
void host_update_bright(bool on) {
    display::noteActivity();
    display::setBrightness(on ? 255 : (uint8_t)s_brightness);
}

// Saved for when idle dimming is ported (a stage 2 leftover); nothing dims yet.
uint32_t host_get_idle_ms() { return s_idleDimMs; }
void host_set_idle_ms(uint32_t ms) {
    s_idleDimMs = ms;
    display::noteActivity();
    orb_prefs::putUInt("idledim", ms);
}

// --- location: location.cpp ------------------------------------------------------------

// --- sound: saved, applied to the audio stub, heard once audio is ported -----------------

int  host_get_volume() { return s_volume; }
void host_set_volume(int v, bool save) {
    s_volume = clamp(v, 0, 100);
    audio_set_volume(s_volume);
    if (save) orb_prefs::putInt("vol", s_volume);
}
int  host_tick_volume() { return s_tickVol; }
void host_set_tick_volume(int v, bool save) {
    s_tickVol = clamp(v, 0, 100);
    audio_set_tick_level(s_tickVol);
    if (save) orb_prefs::putInt("tickVol", s_tickVol);
}
int  host_chime_volume() { return s_chimeVol; }
void host_set_chime_volume(int v, bool save) {
    s_chimeVol = clamp(v, 0, 100);
    audio_set_chime_level(s_chimeVol);
    if (save) orb_prefs::putInt("chimeVol", s_chimeVol);
}
bool host_sound_radar() { return s_soundRadar; }
void host_sound_set_radar(bool on) { s_soundRadar = on; orb_prefs::putBool("sndRadar", on); }
bool host_sound_chime() { return s_soundChime; }
void host_sound_set_chime(bool on) { s_soundChime = on; orb_prefs::putBool("sndChime", on); }
void host_sound_preview_chime() { if (audio_present()) audio_play(AUDIO_CHIME); }
void host_sound_preview_beep()  { if (audio_present()) audio_play(AUDIO_NEW); }
int  host_chime_count()            { return chime_library::count(); }
const char *host_chime_name(int i) { return chime_library::name(i); }
int  host_chime_index()            { return chime_library::selected(); }
void host_chime_set(int i)         { chime_library::select(i); }
void host_chime_preview(int i)     { if (audio_present()) chime_library::preview(i); }

// --- units and range -------------------------------------------------------------------

bool host_wx_is_imperial() {
    if (s_wxUnits == 1) return false;
    if (s_wxUnits == 2) return true;
    return is_imperial_region(flight::homeLat(), flight::homeLon());
}
int  host_wx_units_mode() { return s_wxUnits; }
void host_wx_units_set(int mode) {
    s_wxUnits = clamp(mode, 0, 2);
    orb_prefs::putInt("wxUnits", s_wxUnits);
    ui_set_wx_units(host_wx_is_imperial());
}

float host_get_range_km() { return flight::rangeKm(); }
void  host_set_range_km(float km) { flight::setRangeKm(km); }

// --- WiFi: Settings' list, password and status pages -------------------------------------

void host_wifi_scan_start() { orb_wifi::startScan(); }
int  host_wifi_scan_result(char names[][33], int8_t *rssi, bool *isOpen, int maxN) {
    return orb_wifi::scanResults(names, rssi, isOpen, maxN);
}
void host_wifi_connect(const char *ssid, const char *pass) {
    if (!orb_wifi::tryNetwork(ssid, pass)) printf("[host] a WiFi try is already running\n");
}
// 0 still trying, 1 joined, 2 failed: the values settings_view.cpp's wifi_tick() reads.
int host_wifi_connect_status() {
    switch (orb_wifi::tryResult()) {
        case orb_wifi::TryResult::Joined: return 1;
        case orb_wifi::TryResult::Failed: return 2;
        default:                          return 0;
    }
}
// orb_wifi saves the network itself once it has an address, and on failure has already
// gone back to the owner's own networks; and nothing needs a reboot to use a new network.
void host_wifi_commit_credentials(const char *, const char *) {}
void host_wifi_restore_saved() {}
// Called on a successful join. No reboot; what is left of Arduino's version is clearing
// the setup flag an Arduino Settings > Reset leaves behind.
void host_wifi_connected_reboot() {
    if (orb_prefs::getBool("needsWifiSetup", false)) orb_prefs::putBool("needsWifiSetup", false);
}

// --- reset ---------------------------------------------------------------------------------

// Never erases. Arduino's version wipes the settings namespace and the WiFi driver's, which
// this port has promised never to do (orb_prefs.h); a reset that keeps those promises comes
// with the versioned settings layer in stage 10. Settings stays on its confirm page, and
// turning the knob backs out as usual.
void host_factory_reset() {
    printf("[host] factory reset: not in this build (stage 10); nothing was erased\n");
}
