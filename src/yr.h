#pragma once
// The Weather app's forecast (MET Norway, yr.no's data), shared by the network side that
// fetches it (main/yr.cpp on ESP-IDF, a fixed example in the simulator) and the screen that
// draws it (yr_view.cpp). Celsius, metres a second, millimetres; the screen converts.
#include <stdint.h>

#define YR_HOURS 12      // the next twelve hours, one row each
#define YR_DAYS   7      // today and the six after it
#define YR_SYMBOL_BYTES 24   // MET symbol_code, e.g. "lightrainshowers_night"

struct YrNow {
    float tempC;
    float windMs;
    int   windDeg;        // where the wind blows FROM, degrees
    float precipMm;       // in the next hour
    char  symbol[YR_SYMBOL_BYTES];
};

struct YrHour {
    int   hour;           // local, 0..23
    float tempC;
    float precipMm;
    char  symbol[YR_SYMBOL_BYTES];
};

struct YrDay {
    int   wday;           // local, 0 Sunday .. 6 Saturday
    bool  today;
    float minC, maxC;
    float precipMm;       // the day's total
    char  symbol[YR_SYMBOL_BYTES];   // around midday, as yr.no shows a day
};

struct YrForecast {
    bool     valid;
    uint32_t fetchedMs;   // millis() when it arrived, for "updated n min ago"
    YrNow    now;
    YrHour   hours[YR_HOURS];
    int      nHours;
    YrDay    days[YR_DAYS];
    int      nDays;
};

// Network side writes, the screen reads, under a lock: the whole forecast is under 1 KB.
void yr_store(const YrForecast &f);
bool yr_get(YrForecast &f);
