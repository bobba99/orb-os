// The Orb OS — themes from a computer, over WiFi, for development. See dev_themes.cpp.
#pragma once

namespace dev_themes {

// When CONFIG_ORB_DEV_THEME_URL is set: copy the themes offered there onto the card, once,
// after WiFi is up. Does nothing otherwise. After orb_net::begin() and sdcard::begin().
void start();

}  // namespace dev_themes
