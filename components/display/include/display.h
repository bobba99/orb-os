// The Orb OS — the panel and LVGL. See display.cpp.
#pragma once
#include <stdint.h>

namespace display {

// Bring up the CO5300 over QSPI and start LVGL on it, in a task pinned to core 1.
// False if the panel did not come up; the reason is in the log.
bool begin();

// 0..255, the scale the Arduino firmware stored. 0 is off.
void setBrightness(uint8_t level);

// LVGL runs in its own task, so every LVGL call from anywhere else goes inside a lock.
// lock() returns false on timeout; 0 waits for ever.
bool lock(uint32_t timeoutMs = 0);
void unlock();

}  // namespace display
