// The Orb OS — host.cpp's own interface: the settings it owns, for main.cpp. The host_*
// functions themselves are declared by the src/ files that call them.
#pragma once

namespace host {

// Read the display, sound and units settings from NVS. Once, before the display starts.
void loadSettings();

int brightness();   // the owner's level, 8..255

}  // namespace host
