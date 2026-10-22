// The Orb OS — is the active theme whole? See theme_check.cpp.
#pragma once

namespace theme_check {

// The first thing wrong with the active theme, as a sentence for the screen, or nullptr when
// nothing is (or no theme is active). After theme_select::init() and sdcard::begin().
const char *run();

}  // namespace theme_check
