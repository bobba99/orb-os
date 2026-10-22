// The Orb OS — every HTTPS request the Orb makes, on one client task.
//
// Stage 4 of REWRITE_PLAN.md. One task on core 0 owns esp_http_client and takes requests from
// a queue, so there is only ever one TLS session open: the handshake's buffers are the
// largest single demand on internal RAM, and two at once is what used to run it out.
//
//  - Certificates are verified against ESP-IDF's root bundle. Every service is HTTPS.
//  - The body is not buffered. A request's handler runs on the client task and reads the
//    body as it arrives (Body below is an ArduinoJson reader, so deserializeJson() with a
//    filter parses straight off the socket; XML goes through expat the same way).
//  - Cached requests honour Cache-Control max-age and Expires (no request at all while the
//    last answer is fresh) and revalidate with If-None-Match / If-Modified-Since.
//  - Parsed documents come from a per-app PSRAM allocator with a cap, so one app's runaway
//    answer cannot take the memory another app needs.
//
// What reaches the UI is each app's business: it hands typed results over through its own
// queue or lock (the flight snapshot, route_store()), never LVGL calls from a handler.
//
// fetch() blocks the calling task until its request is done. Two rules follow, and fetch()
// checks the first: it may not be called from the client task (a handler), and a handler
// may never take the display lock, because the UI thread can be the one waiting on fetch().
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <ArduinoJson.h>

namespace orb_net {

// Who asked: for the memory caps and the log.
enum class App : uint8_t { Flight, Routes, Location, Weather, News, Radio, Count };
const char *appName(App a);

// fetch()'s result: an HTTP status (200, 304, 404, ...), or one of these.
constexpr int FRESH        = 1;    // a cached request whose last answer is still fresh: nothing sent
constexpr int ERR_CONNECT  = -1;   // no connection, DNS, or the TLS handshake failed
constexpr int ERR_BODY     = -2;   // the body was cut short, too large, or its handler rejected it
constexpr int ERR_MEMORY   = -3;
constexpr int ERR_OFFLINE  = -4;   // WiFi is not connected; nothing was queued
constexpr int ERR_MISUSE   = -5;   // fetch() from the client task itself

// The response body, read on the client task as it arrives. Reads past the request's maxBytes,
// or after the connection fails, return end-of-input and mark the body failed().
class Body {
public:
    // The ArduinoJson reader interface.
    int    read();
    size_t readBytes(char *buf, size_t n);
    // For anything else (expat, a plain copy): up to n bytes, 0 at the end.
    size_t readChunk(char *buf, size_t n);
    size_t bytesRead() const { return _total; }
    bool   failed() const { return _failed; }

private:
    friend struct Client;
    bool refill();
    void   *_client = nullptr;
    size_t  _max = 0, _total = 0, _pos = 0, _len = 0;
    bool    _failed = false, _ended = false;
    char    _buf[1024];
};

// Runs on the client task for a 200 answer. Return false when the body did not parse; the
// request then reports ERR_BODY.
using Handler = bool (*)(Body &body, void *ctx);

struct Request {
    const char *url       = nullptr;
    App         app       = App::Flight;
    Handler     handler   = nullptr;   // nullptr: the body is read and dropped
    void       *ctx       = nullptr;
    const char *accept    = nullptr;
    const char *postBody  = nullptr;   // set: a POST of this, as contentType
    const char *contentType = "application/json";
    int         timeoutMs = 15000;
    size_t      maxBytes  = 256 * 1024;
    bool        cached    = false;     // honour freshness and revalidate (see above)
};

// Start the client task. After orb_wifi::begin().
void begin();

// Queue the request and wait for its result. See the rules at the top of this file.
int fetch(const Request &req);

// A PSRAM allocator for app `a`'s documents, which refuses an allocation that would take the
// app past its cap. Pass it to JsonDocument's constructor.
ArduinoJson::Allocator *allocator(App a);
size_t memInUse(App a);
size_t memCap(App a);

}  // namespace orb_net
