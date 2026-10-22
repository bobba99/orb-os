// The Orb OS — where the Orb is, on ESP-IDF. See location.cpp. The host_* functions Settings
// and the radar call are declared by the src/ files that call them.
#pragma once

namespace location {

// A device that has never been located asks the network once per boot, and applies the
// answer without changing the screen. Called by the flight network task once WiFi is up.
void locateIfUnset();

}  // namespace location
