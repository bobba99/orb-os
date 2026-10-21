// The Orb OS — WiFi on ESP-IDF, the stage 2 minimum: join the network the Orb already knows.
//
// The network the Arduino firmware saved lives in the WiFi driver's own NVS namespace. The
// driver reads it at init; this then switches the driver to RAM-only storage, so nothing here
// can ever overwrite it. No setup portal, no switching networks, no saving a new one: those
// are stage 3 (feature/wifi-state-machine), which replaces this.
#pragma once
#include <stddef.h>

namespace orb_wifi {

// Start the station with the stored network, if there is one, and keep it connected:
// a dropped link is retried with backoff (1 s doubling to 30 s), for as long as it takes.
// SNTP starts at the first connection. False if WiFi could not be started at all.
bool begin();

bool connected();
int  rssi();                              // dBm, or -127 when not connected
void savedSsid(char *out, size_t n);      // the stored network's name, "" if none
bool timeSynced();                        // SNTP has set the clock at least once

}  // namespace orb_wifi
