// The Orb OS — the platform calls shared code makes, on the builds that are not Arduino.
//
// The UI, theme and app code is compiled by three builds during the ESP-IDF port: the Arduino
// firmware, the desktop simulator and the ESP-IDF firmware (stage 2 of REWRITE_PLAN.md). It
// was written against Arduino, so it logs through Serial, reads millis() and micros(), and
// asks ESP for free PSRAM. The simulator has always answered those with small stand-ins of
// its own in each file. This header answers them for ESP-IDF, in ONE place, with ESP-IDF's
// own calls underneath.
//
// Arduino-compatible names on purpose, for now: they let the shared files compile unchanged
// on all three builds. When the Arduino build retires, these become neutral names (orb_log,
// orb_millis, ...) in one search-and-replace, and this header stays the only place that knows
// what is underneath them.
//
// Only what the shared code actually calls is here. Anything new should be added here, not
// spelled out again in a file.
#pragma once

#if defined(ARDUINO)
#include <Arduino.h>

#elif defined(ESP_PLATFORM)
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Milliseconds and microseconds since boot, from the same timer Arduino's millis() reads.
static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline uint32_t micros() { return (uint32_t)esp_timer_get_time(); }
// As on Arduino-ESP32: a FreeRTOS delay, so other tasks run meanwhile. delay(0) yields.
static inline void delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

// The console. ESP-IDF's stdout is the board's USB-Serial/JTAG port (sdkconfig.defaults),
// which is where Serial wrote on Arduino.
struct OrbSerial {
    int printf(const char *fmt, ...) const __attribute__((format(printf, 2, 3))) {
        va_list a;
        va_start(a, fmt);
        const int n = vprintf(fmt, a);
        va_end(a);
        return n;
    }
    // Arduino ends a println with CRLF; so does this, so a log reads the same on both.
    size_t println(const char *s = "") const { return (size_t)::printf("%s\r\n", s); }
};
static const OrbSerial Serial;

struct OrbEsp {
    uint32_t getFreePsram() const { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
    uint32_t getFreeHeap() const { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
    [[noreturn]] void restart() const { esp_restart(); }
};
static const OrbEsp ESP;

// Where components/orb_sdcard mounts the card. The shared code's POSIX file paths, which the
// simulator roots at "sim/sdcard", are rooted here on the device.
#define ORB_SD_ROOT "/sdcard"

#endif
