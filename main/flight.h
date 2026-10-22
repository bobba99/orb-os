// The Orb OS — the Flight Tracker's host side on ESP-IDF: its settings, the network task that
// feeds it, and the hand-off to the radar. See flight.cpp.
#pragma once
#include <stdint.h>

namespace flight {

// After theme_select::init() and ui_create(): read the scope's settings from NVS (the Arduino
// keys), let the theme override the ones it has an opinion on, and apply them to the radar.
// Call with the display lock held.
void applySettings();

// Start the network task on core 0. After orb_wifi::begin().
void begin();

// The app shell's enter/exit for Flight Tracker: polling runs at full rate while it is on
// screen, keeps warm for ten minutes after, then stops until it is opened again.
void setVisible(bool on);

// Core 1, display lock held, every loop pass: draw a new snapshot if one arrived, and the
// catch-up frame when the tracker has just come on screen.
void loopTick();

// Core 1, display lock held, on the 5-second status tick: the HUD's feed state and the
// scope's "No WiFi" / "Location not set" / "No aircraft data" notice.
void statusTick(bool wifiUp);

uint32_t lastFeedOkMs();   // millis() of the last good poll, 0 if none yet

double homeLat();
double homeLon();

// Settings > Range. src/app_hooks.cpp onRangeChange(): saved, the scope redrawn at once from
// the last snapshot, and the feed asked again at the new radius. Display lock held.
float rangeKm();
void  setRangeKm(float km);

}  // namespace flight
