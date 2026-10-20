// The Orb OS — the PCF85063 real-time clock on ESP-IDF. Implements src/rtc_pcf85063.h.
//
// A port of src/rtc_pcf85063.cpp, register for register: the same seven-byte time block, the
// same oscillator-stopped check and the same "before 2024 means never set" rule. Only the bus
// calls changed, from Wire to the shared orb_i2c bus.
#include "rtc_pcf85063.h"
#include "config.h"
#include "orb_i2c.h"
#include <stdio.h>

// PCF85063 register map (time block starts at 0x04):
#define REG_SECONDS 0x04   // bit7 = VL: 1 => oscillator stopped, time not guaranteed
// 0x05 minutes, 0x06 hours(24h), 0x07 days, 0x08 weekday, 0x09 months, 0x0A years(00-99, base 2000)

static constexpr int RTC_TIMEOUT_MS = 50;

static bool s_ok = false;
static i2c_master_dev_handle_t s_dev = nullptr;

static inline uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

bool rtc_begin() {
    s_ok = orb_i2c::present(I2C_ADDR_RTC);
    if (s_ok) s_dev = orb_i2c::device(I2C_ADDR_RTC);
    s_ok = s_ok && s_dev;
    printf("[rtc] PCF85063 %s\n", s_ok ? "ready" : "not found");
    return s_ok;
}

bool rtc_present() { return s_ok; }

bool rtc_read(struct tm *out) {
    if (!s_ok || !out) return false;
    const uint8_t reg = REG_SECONDS;
    uint8_t b[7];
    // Register address, repeated start, seven bytes: what endTransmission(false) + requestFrom did.
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof(b), RTC_TIMEOUT_MS) != ESP_OK) return false;
    const uint8_t s = b[0], mi = b[1], h = b[2], d = b[3], mo = b[5], y = b[6];   // b[4]: weekday, unused
    if (s & 0x80) return false;                                  // VL set => invalid
    out->tm_sec   = bcd2dec(s  & 0x7F);
    out->tm_min   = bcd2dec(mi & 0x7F);
    out->tm_hour  = bcd2dec(h  & 0x3F);
    out->tm_mday  = bcd2dec(d  & 0x3F);
    out->tm_mon   = bcd2dec(mo & 0x1F) - 1;
    out->tm_year  = bcd2dec(y) + 100;                            // tm_year = years since 1900
    out->tm_isdst = 0;
    return out->tm_year >= 124;                                  // < 2024 => never set
}

bool rtc_write(const struct tm *t) {
    if (!s_ok || !t) return false;
    const uint8_t b[8] = {
        REG_SECONDS,
        (uint8_t)(dec2bcd((uint8_t)t->tm_sec) & 0x7F),           // also clears VL
        dec2bcd((uint8_t)t->tm_min),
        dec2bcd((uint8_t)t->tm_hour),
        dec2bcd((uint8_t)t->tm_mday),
        0,                                                       // weekday (unused)
        dec2bcd((uint8_t)(t->tm_mon + 1)),
        dec2bcd((uint8_t)(t->tm_year - 100)),
    };
    return i2c_master_transmit(s_dev, b, sizeof(b), RTC_TIMEOUT_MS) == ESP_OK;
}
