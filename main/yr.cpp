// The Orb OS — the Weather app's forecast from MET Norway, on ESP-IDF. Stage 8.
//
// Locationforecast 2.0 "compact" for the Orb's location, through orb_net: HTTPS, its cache
// (MET sends Expires and Last-Modified, and asks clients to honour both), and a JSON filter so
// only what the screen uses is kept. MET's terms, kept here: an identifying User-Agent
// (ORB_USER_AGENT names this firmware and where it lives), coordinates to four decimals, and
// no request more often than the forecast changes. The screen credits MET (yr_view.cpp).
//
// What the screen gets (src/yr.h):
//  - now: the first time step's temperature and wind, the next hour's symbol and rain;
//  - hours: the next twelve, local time;
//  - days: today and six more, local dates: the lowest and highest temperature of the day's
//    steps, the day's rain (each step's own period, so no hour is counted twice: hourly while
//    MET gives hours, then six-hourly) and the symbol of the six hours around midday.
#include "yr.h"
#include "yr_fetch.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ArduinoJson.h>
#include "esp_timer.h"
#include "lvgl.h"

#include "flight.h"
#include "orb_net.h"
#include "orb_platform.h"

namespace {

constexpr int64_t POLL_US = 10LL * 60 * 1000000;   // ask again after ten minutes; MET's Expires decides
int64_t s_lastUs = 0;
double  s_lat = 1000, s_lon = 1000;                // where the stored forecast is for

// "2026-10-22T20:00:00Z" to seconds since 1970 (UTC), or -1.
int64_t parse_iso(const char *s) {
    int y, m, d, hh, mm, ss;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &hh, &mm, &ss) != 6) return -1;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return ((int64_t)era * 146097 + doe - 719468) * 86400 + hh * 3600 + mm * 60 + ss;
}

void copy_symbol(char *dst, const char *src) { snprintf(dst, YR_SYMBOL_BYTES, "%s", src ? src : ""); }

struct Parse { JsonDocument *doc; JsonDocument *filter; DeserializationError err; };

bool parse(orb_net::Body &body, void *ctx) {
    Parse *p = (Parse *)ctx;
    p->err = deserializeJson(*p->doc, body, DeserializationOption::Filter(*p->filter));
    return !p->err;
}

// The document into the screen's forecast. False when it holds no time steps.
bool build(JsonDocument &doc, YrForecast &f) {
    JsonArrayConst ts = doc["properties"]["timeseries"].as<JsonArrayConst>();
    if (ts.isNull() || ts.size() == 0) return false;
    memset(&f, 0, sizeof(f));

    const time_t nowT = time(nullptr);
    struct tm today;
    localtime_r(&nowT, &today);
    int dayKey[YR_DAYS];            // year*1000 + day of year, per day slot
    int bestNoon[YR_DAYS];          // how far that day's symbol step is from midday, in hours
    bool seen[YR_DAYS] = {};        // the day has had a step
    for (int i = 0; i < YR_DAYS; ++i) { dayKey[i] = -1; bestNoon[i] = 99; }

    bool first = true;
    for (JsonObjectConst step : ts) {
        const int64_t t = parse_iso(step["time"] | "");
        if (t < 0) continue;
        JsonObjectConst data = step["data"];
        const float temp = data["instant"]["details"]["air_temperature"] | NAN;
        const char *sym1 = data["next_1_hours"]["summary"]["symbol_code"] | "";
        const char *sym6 = data["next_6_hours"]["summary"]["symbol_code"] | "";
        const char *sym12 = data["next_12_hours"]["summary"]["symbol_code"] | "";
        const bool hourly = !data["next_1_hours"].isNull();
        const float rain = hourly ? (data["next_1_hours"]["details"]["precipitation_amount"] | 0.0f)
                                  : (data["next_6_hours"]["details"]["precipitation_amount"] | 0.0f);
        const time_t tt = (time_t)t;
        struct tm lt;
        localtime_r(&tt, &lt);

        if (first) {
            first = false;
            f.now.tempC = temp;
            f.now.windMs = data["instant"]["details"]["wind_speed"] | 0.0f;
            f.now.windDeg = (int)lroundf(data["instant"]["details"]["wind_from_direction"] | 0.0f);
            f.now.precipMm = hourly ? rain : 0.0f;
            copy_symbol(f.now.symbol, sym1[0] ? sym1 : sym6[0] ? sym6 : sym12);
        } else if (hourly && f.nHours < YR_HOURS && !isnan(temp)) {
            YrHour &h = f.hours[f.nHours++];
            h.hour = lt.tm_hour;
            h.tempC = temp;
            h.precipMm = rain;
            copy_symbol(h.symbol, sym1);
        }

        // Which day slot: today is 0. Steps past the seventh day are not wanted.
        const int key = lt.tm_year * 1000 + lt.tm_yday;
        int d = -1;
        for (int i = 0; i < YR_DAYS; ++i) {
            if (dayKey[i] == key) { d = i; break; }
            if (dayKey[i] < 0) { dayKey[i] = key; d = i; f.nDays = i + 1; break; }
        }
        if (d < 0) break;
        YrDay &day = f.days[d];
        if (!seen[d]) {
            seen[d] = true;
            day.wday = lt.tm_wday;
            day.today = lt.tm_year == today.tm_year && lt.tm_yday == today.tm_yday;
            day.minC = day.maxC = temp;
        }
        if (!isnan(temp)) {
            if (temp < day.minC || isnan(day.minC)) day.minC = temp;
            if (temp > day.maxC || isnan(day.maxC)) day.maxC = temp;
        }
        day.precipMm += rain;
        const int fromNoon = abs(lt.tm_hour - 12);
        const char *daySym = sym6[0] ? sym6 : sym12;
        if (daySym[0] && fromNoon < bestNoon[d]) {
            bestNoon[d] = fromNoon;
            copy_symbol(day.symbol, daySym);
        }
    }
    f.valid = true;
    return true;
}

}  // namespace

void yr_service() {
    if (!flight::locationSet()) return;
    const double lat = flight::homeLat(), lon = flight::homeLon();
    const bool moved = fabs(lat - s_lat) > 0.00005 || fabs(lon - s_lon) > 0.00005;
    const int64_t now = esp_timer_get_time();
    if (!moved && s_lastUs && now - s_lastUs < POLL_US) return;
    s_lastUs = now;

    char url[128];
    snprintf(url, sizeof(url), "https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=%.4f&lon=%.4f", lat, lon);
    JsonDocument filter;
    JsonObject step = filter["properties"]["timeseries"].add<JsonObject>();
    step["time"] = true;
    JsonObject data = step["data"].to<JsonObject>();
    for (const char *k : { "air_temperature", "wind_speed", "wind_from_direction" })
        data["instant"]["details"][k] = true;
    for (const char *k : { "next_1_hours", "next_6_hours", "next_12_hours" }) {
        data[k]["summary"]["symbol_code"] = true;
        data[k]["details"]["precipitation_amount"] = true;
    }
    JsonDocument doc(orb_net::allocator(orb_net::App::Weather));
    Parse p{ &doc, &filter, {} };
    orb_net::Request req;
    req.url = url;
    req.app = orb_net::App::Weather;
    req.accept = "application/json";
    req.timeoutMs = 15000;
    req.maxBytes = 128 * 1024;
    req.cached = !moved;     // a new place has nothing cached worth revalidating
    req.handler = parse;
    req.ctx = &p;
    const int status = orb_net::fetch(req);
    if (status == 304 || status == orb_net::FRESH) {
        printf("[yr] forecast unchanged (%d)\n", status);
        return;
    }
    if (status != 200) {
        printf("[yr] forecast failed (%d%s%s)\n", status, p.err ? ", " : "", p.err ? p.err.c_str() : "");
        s_lastUs = now - POLL_US + 60LL * 1000000;   // try again in a minute
        return;
    }
    static YrForecast f;
    if (!build(doc, f)) { printf("[yr] the forecast held no time steps\n"); return; }
    f.fetchedMs = lv_tick_get();   // the clock yr_view measures the age with
    yr_store(f);
    s_lat = lat;
    s_lon = lon;
    printf("[yr] %.1f C, %s, wind %.1f m/s; %d hours, %d days\n", (double)f.now.tempC, f.now.symbol,
           (double)f.now.windMs, f.nHours, f.nDays);
}
