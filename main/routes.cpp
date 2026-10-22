// The Orb OS — flight routes on ESP-IDF: the airports a callsign flies between.
//
// The Arduino firmware asked the Studio gateway, which is gone. adsb.lol publishes the same
// route data as one small static file per callsign (vrs-standing-data.adsb.lol; its
// /api/0/route endpoint now redirects there), with an ETag and ten minutes of freshness, so
// the request goes through orb_net's cache: inside ten minutes nothing is sent, after that a
// conditional request usually comes back 304. The answers themselves are kept here, in RAM,
// where Arduino kept them in an NVS namespace it wiped whenever it filled (this port never
// erases NVS).
//
// What is shown is the first and last IATA code ("BGO" -> "AMS"), as the gateway gave it.
#include "routes.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <ArduinoJson.h>

#include "esp_timer.h"
#include "orb_net.h"
#include "route.h"

namespace {

struct Known { char call[12]; char from[8]; char to[8]; int64_t usedUs; };
// More than orb_net's cache holds, so a URL it calls fresh always has its answer here.
constexpr int KNOWN_N = 32;
Known s_known[KNOWN_N];

Known *find(const char *call) {
    for (Known &k : s_known) if (k.call[0] && !strcmp(k.call, call)) return &k;
    return nullptr;
}

Known *slot(const char *call) {
    if (Known *k = find(call)) return k;
    Known *oldest = &s_known[0];
    for (Known &k : s_known) {
        if (!k.call[0]) { oldest = &k; break; }
        if (k.usedUs < oldest->usedUs) oldest = &k;
    }
    *oldest = Known{};
    snprintf(oldest->call, sizeof(oldest->call), "%s", call);
    return oldest;
}

struct Answer { char from[8]; char to[8]; };

// {"_airport_codes_iata":"BGO-AMS", ...}; a multi-leg flight lists every stop.
bool parse(orb_net::Body &body, void *ctx) {
    Answer *a = (Answer *)ctx;
    JsonDocument filter;
    filter["_airport_codes_iata"] = true;
    JsonDocument doc(orb_net::allocator(orb_net::App::Routes));
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
    const char *codes = doc["_airport_codes_iata"] | "";
    const char *dash = strchr(codes, '-');
    const char *last = strrchr(codes, '-');
    if (!dash) return true;   // known callsign, no route
    snprintf(a->from, sizeof(a->from), "%.*s", (int)(dash - codes), codes);
    snprintf(a->to, sizeof(a->to), "%s", last + 1);
    return true;
}

// One fetch. Returns false when the network said nothing usable this time.
bool lookup(const char *call, bool cached, Answer &a, int &status) {
    char url[96];
    snprintf(url, sizeof(url), "https://vrs-standing-data.adsb.lol/routes/%.2s/%s.json", call, call);
    a = Answer{};
    orb_net::Request req;
    req.url = url;
    req.app = orb_net::App::Routes;
    req.accept = "application/json";
    req.timeoutMs = 6000;
    req.maxBytes = 8 * 1024;
    req.cached = cached;
    req.handler = parse;
    req.ctx = &a;
    status = orb_net::fetch(req);
    return status == 200 || status == 304 || status == orb_net::FRESH || status == 404;
}

}  // namespace

void routes::service() {
    char want[12];
    if (!route_pending(want, sizeof(want))) return;
    // Callsigns are letters and digits; the feed pads them with spaces.
    char call[12];
    size_t n = 0;
    for (const char *p = want; *p && n < sizeof(call) - 1; ++p)
        if (isalnum((unsigned char)*p)) call[n++] = (char)toupper((unsigned char)*p);
    call[n] = 0;
    if (n < 3) { route_store(want, "", ""); return; }

    Answer a;
    int status = 0;
    bool ok = lookup(call, true, a, status);
    Known *k = find(call);
    if (ok && (status == 304 || status == orb_net::FRESH) && !k) ok = lookup(call, false, a, status);
    if (!ok) {
        route_store(want, "", "");   // as Arduino: no retry this session for this callsign
        printf("[route] %s: lookup failed (%d)\n", call, status);
        return;
    }
    if (status == 200 || status == 404) {
        k = slot(call);
        snprintf(k->from, sizeof(k->from), "%s", a.from);
        snprintf(k->to, sizeof(k->to), "%s", a.to);
    }
    k = find(call);
    k->usedUs = esp_timer_get_time();
    route_store(want, k->from, k->to);
    printf("[route] %s: '%s' -> '%s' (%d)\n", call, k->from, k->to, status);
}
