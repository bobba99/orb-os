// The Orb OS — where the Orb is, on ESP-IDF: the IP lookup, the city search, recent places,
// and the timezone that follows a located position.
//
// src/location.cpp's behaviour, through orb_net and over HTTPS, with three differences:
//  - ip-api.com answers HTTPS only for paying customers, so the IP lookup asks ipapi.co, and
//    ipwho.is when ipapi.co refuses (it rate-limits shared addresses).
//  - A place is applied while the Orb runs. Arduino saved it and restarted; here the scope
//    moves and the Flight Tracker comes up to show it.
//  - The timezone keeps its daylight-saving rule when the zone is one the table below knows.
//    Arduino stored a fixed offset, which goes an hour wrong at the next change.
#include "location.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ArduinoJson.h>

#include "orb_net.h"
#include "orb_prefs.h"
#include "flight.h"
#include "app_shell.h"

void host_recents_add(const char *name, double lat, double lon);   // below

namespace {

// --- timezones -------------------------------------------------------------------------

// IANA name to POSIX rule, for the zones an Orb is likely to find itself in. Anything else
// gets the fixed offset the lookup reports, as Arduino did for every zone.
struct Zone { const char *iana, *posix; };
const Zone ZONES[] = {
    { "Europe/Oslo",       "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Stockholm",  "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Berlin",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Amsterdam",  "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Brussels",   "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Paris",      "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Madrid",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome",       "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Vienna",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Zurich",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Warsaw",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Prague",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Budapest",   "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/London",     "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Dublin",     "GMT0IST,M3.5.0/1,M10.5.0" },
    { "Europe/Lisbon",     "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Atlantic/Reykjavik","GMT0" },
    { "Europe/Helsinki",   "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Tallinn",    "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Riga",       "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Vilnius",    "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Athens",     "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "America/New_York",  "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Toronto",   "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Chicago",   "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Denver",    "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Phoenix",   "MST7" },
    { "America/Los_Angeles","PST8PDT,M3.2.0,M11.1.0" },
    { "America/Vancouver", "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
    { "Pacific/Honolulu",  "HST10" },
    { "Australia/Sydney",  "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Melbourne","AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Brisbane","AEST-10" },
    { "Australia/Perth",   "AWST-8" },
    { "Pacific/Auckland",  "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "Asia/Tokyo",        "JST-9" },
};

// src/location.cpp posix_tz_from_offset(): a fixed offset, no daylight saving.
void posix_from_offset(long offsetSec, char *out, size_t n) {
    const long offMin  = offsetSec / 60;
    const long westMin = -offMin;
    const char sign = offMin < 0 ? '-' : '+';
    const int aH = (int)(labs(offMin) / 60), aM = (int)(labs(offMin) % 60);
    const int wh = (int)(westMin / 60),       wm = (int)(labs(westMin) % 60);
    if (aM == 0) snprintf(out, n, "<%c%02d>%d", sign, aH, wh);
    else         snprintf(out, n, "<%c%02d%02d>%d:%02d", sign, aH, aM, wh, wm);
}

// Saved under the Arduino key and applied now: the clock reads local from the next second.
void apply_timezone(const char *iana, long offsetSec, bool haveOffset) {
    char tz[48] = "";
    for (const Zone &z : ZONES)
        if (iana && !strcmp(iana, z.iana)) { snprintf(tz, sizeof(tz), "%s", z.posix); break; }
    if (!tz[0]) {
        if (!haveOffset || offsetSec < -50400 || offsetSec > 50400) return;
        posix_from_offset(offsetSec, tz, sizeof(tz));
    }
    orb_prefs::putString("tz", tz);
    setenv("TZ", tz, 1);
    tzset();
    printf("[locate] timezone %s -> %s\n", iana && iana[0] ? iana : "(offset only)", tz);
}

// --- saving and applying a position ----------------------------------------------------

void persist_name(const char *name) {
    if (name && name[0]) orb_prefs::putString("homeName", name);
}

// src/location.cpp persist_location() + apply_location_live().
void persist_and_apply(double lat, double lon) {
    orb_prefs::putDouble("homeLat", lat);
    orb_prefs::putDouble("homeLon", lon);
    orb_prefs::putBool("locSet", true);
    if (orb_prefs::getBool("needsWifiSetup", false)) orb_prefs::putBool("needsWifiSetup", false);
    flight::setHome(lat, lon);
    printf("[locate] home is now %.4f, %.4f\n", lat, lon);
}

// Somebody picked this place in Settings: show them the scope on it.
void show_flight() {
    app_shell::setCaptured(false);
    app_shell::selectApp(app_shell::APP_FLIGHT);
}

// --- the IP lookup ---------------------------------------------------------------------

struct Located {
    double lat = NAN, lon = NAN;
    char   name[40] = "";
    char   zone[40] = "";
    long   offsetSec = 0;
    bool   haveOffset = false;
};

bool valid(double la, double lo) { return la >= -90 && la <= 90 && lo >= -180 && lo <= 180; }

void make_name(char *out, size_t n, const char *city, const char *region) {
    snprintf(out, n, "%s%s%s", city, (city[0] && region[0]) ? ", " : "", city[0] ? region : "");
}

// {"city":"Stavanger","region":"Rogaland","latitude":58.97,"longitude":5.73,
//  "timezone":"Europe/Oslo","utc_offset":"+0200"}, or {"error":true,"reason":"RateLimited"}.
bool parse_ipapi(orb_net::Body &body, void *ctx) {
    Located *L = (Located *)ctx;
    JsonDocument doc(orb_net::allocator(orb_net::App::Location));
    if (deserializeJson(doc, body)) return false;
    if (doc["error"] | false) {
        printf("[locate] ipapi.co: %s\n", (const char *)(doc["reason"] | "error"));
        return true;   // an answer, just not a position
    }
    const double la = doc["latitude"] | NAN, lo = doc["longitude"] | NAN;
    if (!valid(la, lo)) return true;
    L->lat = la; L->lon = lo;
    make_name(L->name, sizeof(L->name), doc["city"] | "", doc["region"] | "");
    snprintf(L->zone, sizeof(L->zone), "%s", (const char *)(doc["timezone"] | ""));
    const char *off = doc["utc_offset"] | "";
    int h = 0, m = 0;
    if ((off[0] == '+' || off[0] == '-') && sscanf(off + 1, "%2d%2d", &h, &m) == 2) {
        L->offsetSec = (off[0] == '-' ? -1 : 1) * (h * 3600L + m * 60L);
        L->haveOffset = true;
    }
    return true;
}

// {"success":true,"city":"Stavanger","region":"Rogaland Fylke","latitude":58.97,
//  "longitude":5.73,"timezone":{"id":"Europe/Oslo","offset":7200}}
bool parse_ipwho(orb_net::Body &body, void *ctx) {
    Located *L = (Located *)ctx;
    JsonDocument filter;
    for (const char *k : { "success", "message", "city", "region", "latitude", "longitude" }) filter[k] = true;
    filter["timezone"]["id"] = true;
    filter["timezone"]["offset"] = true;
    JsonDocument doc(orb_net::allocator(orb_net::App::Location));
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
    if (!(doc["success"] | false)) {
        printf("[locate] ipwho.is: %s\n", (const char *)(doc["message"] | "no answer"));
        return true;
    }
    const double la = doc["latitude"] | NAN, lo = doc["longitude"] | NAN;
    if (!valid(la, lo)) return true;
    L->lat = la; L->lon = lo;
    make_name(L->name, sizeof(L->name), doc["city"] | "", doc["region"] | "");
    snprintf(L->zone, sizeof(L->zone), "%s", (const char *)(doc["timezone"]["id"] | ""));
    if (!doc["timezone"]["offset"].isNull()) {
        L->offsetSec = doc["timezone"]["offset"].as<long>();
        L->haveOffset = true;
    }
    return true;
}

// Ask the network where it is. The name goes to recents and the timezone is applied here,
// because both are true the moment the answer arrives, however the position is then used.
bool ip_lookup(Located &L) {
    struct Source { const char *url; orb_net::Handler parse; };
    const Source sources[] = { { "https://ipapi.co/json/", parse_ipapi },
                               { "https://ipwho.is/", parse_ipwho } };
    for (const Source &s : sources) {
        L = Located{};
        orb_net::Request req;
        req.url = s.url;
        req.app = orb_net::App::Location;
        req.accept = "application/json";
        req.timeoutMs = 8000;
        req.maxBytes = 8 * 1024;
        req.handler = s.parse;
        req.ctx = &L;
        if (orb_net::fetch(req) == 200 && valid(L.lat, L.lon)) {
            if (L.name[0]) { host_recents_add(L.name, L.lat, L.lon); persist_name(L.name); }
            apply_timezone(L.zone, L.offsetSec, L.haveOffset);
            return true;
        }
    }
    printf("[locate] the IP lookup failed\n");
    return false;
}

}  // namespace

// --- the host functions Settings and the radar call --------------------------------------

bool host_location_name(char *out, size_t n) {
    if (!n) return false;
    return orb_prefs::getString("homeName", out, n) && out[0];
}

void host_set_location(double lat, double lon) {
    persist_and_apply(lat, lon);
    show_flight();
}

void host_set_location_named(const char *name, double lat, double lon) {
    host_recents_add(name, lat, lon);
    persist_name(name);
    host_set_location(lat, lon);
}

// Settings > Location > Current, and the end of first-time WiFi setup. On the UI thread, which
// waits for the answer, as Arduino's did; Settings has painted "Locating..." first.
bool host_locate_current() {
    Located L;
    if (!ip_lookup(L)) return false;
    host_set_location(L.lat, L.lon);
    return true;
}

void location::locateIfUnset() {
    if (flight::locationSet()) return;
    Located L;
    if (!ip_lookup(L)) {
        printf("[locate] no location set and the lookup failed; the scope will say so until "
               "one is picked in Settings\n");
        return;
    }
    persist_and_apply(L.lat, L.lon);   // nobody asked, so no change of screen
}

// The city search (Open-Meteo, no key). Settings calls it once per pause in typing, on the UI
// thread, and has painted "searching" first.
int host_geocode(const char *query, char names[][40], double *lats, double *lons, int maxN) {
    if (!query || strlen(query) < 2 || maxN <= 0) return 0;
    char q[96] = "";
    size_t o = 0;
    for (const char *p = query; *p && o + 4 < sizeof(q); ++p) {
        const unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') q[o++] = (char)c;
        else o += snprintf(q + o, sizeof(q) - o, "%%%02X", c);
    }
    q[o] = 0;
    char url[192];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d&language=en&format=json",
             q, maxN);

    struct Out { char (*names)[40]; double *lats, *lons; int maxN, n; } out{names, lats, lons, maxN, 0};
    orb_net::Request req;
    req.url = url;
    req.app = orb_net::App::Location;
    req.accept = "application/json";
    req.timeoutMs = 8000;
    req.maxBytes = 32 * 1024;
    req.ctx = &out;
    req.handler = [](orb_net::Body &body, void *ctx) {
        Out *o = (Out *)ctx;
        JsonDocument filter;
        for (const char *k : { "name", "admin1", "country_code", "latitude", "longitude" })
            filter["results"][0][k] = true;
        JsonDocument doc(orb_net::allocator(orb_net::App::Location));
        if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
        for (JsonObjectConst r : doc["results"].as<JsonArrayConst>()) {
            if (o->n >= o->maxN) break;
            const char *name = r["name"] | "", *admin1 = r["admin1"] | "", *cc = r["country_code"] | "";
            const char *sub = admin1[0] ? admin1 : cc;
            const double la = r["latitude"] | 1000.0, lo = r["longitude"] | 1000.0;
            if (!valid(la, lo)) continue;
            snprintf(o->names[o->n], 40, "%s%s%s", name, sub[0] ? ", " : "", sub);
            o->lats[o->n] = la;
            o->lons[o->n] = lo;
            ++o->n;
        }
        return true;
    };
    return orb_net::fetch(req) == 200 ? out.n : 0;
}

// --- recent places: the Arduino NVS key and format ("recents", a JSON array) -----------------

namespace { constexpr int RECENTS_MAX = 8; }

int host_recents_get(char names[][40], double *lats, double *lons, int maxN) {
    char blob[768];
    if (!orb_prefs::getString("recents", blob, sizeof(blob)) || !blob[0]) return 0;
    JsonDocument doc;
    if (deserializeJson(doc, blob)) return 0;
    int n = 0;
    for (JsonObjectConst r : doc.as<JsonArrayConst>()) {
        if (n >= maxN) break;
        snprintf(names[n], 40, "%s", (const char *)(r["n"] | ""));
        lats[n] = r["la"] | 1000.0;
        lons[n] = r["lo"] | 1000.0;
        if (names[n][0] && lats[n] <= 90 && lats[n] >= -90) n++;
    }
    return n;
}

void host_recents_add(const char *name, double lat, double lon) {
    if (!name || !name[0]) return;
    char   names[RECENTS_MAX][40];
    double lats[RECENTS_MAX], lons[RECENTS_MAX];
    const int n = host_recents_get(names, lats, lons, RECENTS_MAX);
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    { JsonObject o = arr.add<JsonObject>(); o["n"] = name; o["la"] = lat; o["lo"] = lon; }
    int count = 1;
    for (int i = 0; i < n && count < RECENTS_MAX; ++i) {
        if (strcmp(names[i], name) == 0) continue;
        JsonObject o = arr.add<JsonObject>();
        o["n"] = names[i]; o["la"] = lats[i]; o["lo"] = lons[i];
        count++;
    }
    char out[768];
    if (measureJson(doc) >= sizeof(out)) return;   // eight names of 40 never come near; never truncate
    serializeJson(doc, out, sizeof(out));
    orb_prefs::putString("recents", out);
}
