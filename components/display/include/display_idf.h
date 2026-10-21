// The Orb OS — what the ESP-IDF build needs from the display beyond src/display.h.
//
// src/display.h is the contract the shared UI code calls, and it is the Arduino firmware's
// too, so nothing ESP-IDF-specific goes in it. This is the one addition: LVGL renders in
// esp_lvgl_port's task here, where on Arduino it ran inside loop(), so the main loop takes
// this lock for its whole pass and lets go only while it sleeps. Every LVGL call the shared
// code makes is then serialised with the renderer exactly as it was on Arduino.
#pragma once
#include <stdint.h>

namespace display_idf {
bool lock(uint32_t timeoutMs = 0);   // 0 waits for ever; false on timeout
void unlock();
}  // namespace display_idf
