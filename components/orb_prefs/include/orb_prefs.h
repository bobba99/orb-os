// The Orb OS — saved settings on ESP-IDF, in the same NVS keys the Arduino firmware uses.
//
// The ONLY code in the ESP-IDF build that touches NVS. Two promises are kept here, in the
// one path every caller goes through, rather than in a comment beside each caller:
//
//  1. The namespace is "capsuleradar", always. Callers cannot name another. Renaming it would
//     make every Orb look factory reset (CLAUDE.md, REWRITE_PLAN.md "must not break").
//  2. NVS is never erased. The usual ESP-IDF start-up recipe, and Arduino's own initArduino(),
//     erase the whole partition when it reports no free pages or a newer format. That
//     partition also holds the WiFi driver's saved network, so the recipe trades a settings
//     problem for a stranger re-entering their WiFi password. If NVS will not initialise,
//     begin() says so and every read returns its default; nothing is ever written or wiped.
//
// Values are stored with Arduino Preferences' encodings (int as i32, uint as u32, a 64-bit
// int as i64, bool as u8, string as an NVS string), so either firmware can be flashed over
// the other and find what the other saved.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace orb_prefs {

bool begin();      // call once at boot; false = NVS unusable, defaults only, nothing written
bool available();  // begin() succeeded

// Reads: the stored value, or the default when the key is absent, of another type, or NVS is
// unavailable. getString always leaves `out` NUL-terminated, and returns false on a default.
bool     getString(const char *key, char *out, size_t outLen, const char *def = "");
int32_t  getInt(const char *key, int32_t def);
uint32_t getUInt(const char *key, uint32_t def);
int64_t  getInt64(const char *key, int64_t def);   // Arduino putLong64 / getLong64
bool     getBool(const char *key, bool def);
bool     isKey(const char *key);

// Writes, committed before they return. False if the write did not happen.
bool putString(const char *key, const char *value);
bool putInt(const char *key, int32_t value);
bool putUInt(const char *key, uint32_t value);
bool putInt64(const char *key, int64_t value);
bool putBool(const char *key, bool value);
bool remove(const char *key);

}  // namespace orb_prefs
