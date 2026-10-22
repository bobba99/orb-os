// The Orb OS — the one HTTPS client task. See orb_net.h.
#include "orb_net.h"

#include <atomic>
#include <new>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"

#include "config.h"      // ORB_USER_AGENT
#include "orb_wifi.h"

namespace orb_net {

// --- per-app memory -------------------------------------------------------------------------

namespace {

// Generous against what each app is known to need, and there to stop a runaway answer, not to
// ration a normal one. The aircraft document is filtered to the fields the scope uses.
constexpr size_t CAPS[(int)App::Count] = {
    512 * 1024,   // Flight
     16 * 1024,   // Routes
     32 * 1024,   // Location
    256 * 1024,   // Weather
    256 * 1024,   // News
     64 * 1024,   // Radio
     32 * 1024,   // Themes: the index of a development theme server (main/dev_themes.cpp)
};
const char *const NAMES[(int)App::Count] = { "flight", "routes", "location", "weather", "news", "radio", "themes" };

// Each block carries its size in front of it, so a free or a grow knows what it gives back.
constexpr size_t HDR = 8;   // keeps the block 8-byte aligned, as malloc's is

struct CappedAllocator : ArduinoJson::Allocator {
    App app;
    std::atomic<size_t> used{0};
    std::atomic<bool> warned{false};

    bool take(size_t n) {
        size_t cur = used.load();
        do {
            if (cur + n > CAPS[(int)app]) {
                if (!warned.exchange(true))
                    printf("[net] %s: over its %u KB memory cap; the document is refused\n",
                           NAMES[(int)app], (unsigned)(CAPS[(int)app] / 1024));
                return false;
            }
        } while (!used.compare_exchange_weak(cur, cur + n));
        return true;
    }
    void *allocate(size_t n) override {
        if (!take(n)) return nullptr;
        uint8_t *p = (uint8_t *)heap_caps_malloc(n + HDR, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) { used -= n; return nullptr; }
        *(size_t *)p = n;
        return p + HDR;
    }
    void deallocate(void *ptr) override {
        if (!ptr) return;
        uint8_t *p = (uint8_t *)ptr - HDR;
        used -= *(size_t *)p;
        heap_caps_free(p);
    }
    void *reallocate(void *ptr, size_t n) override {
        if (!ptr) return allocate(n);
        uint8_t *p = (uint8_t *)ptr - HDR;
        const size_t old = *(size_t *)p;
        if (n > old && !take(n - old)) return nullptr;
        uint8_t *q = (uint8_t *)heap_caps_realloc(p, n + HDR, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!q) { if (n > old) used -= n - old; return nullptr; }
        if (n < old) used -= old - n;
        *(size_t *)q = n;
        return q + HDR;
    }
};

CappedAllocator s_alloc[(int)App::Count];

}  // namespace

const char *appName(App a) { return (int)a < (int)App::Count ? NAMES[(int)a] : "?"; }
ArduinoJson::Allocator *allocator(App a) { return &s_alloc[(int)a]; }
size_t memInUse(App a) { return s_alloc[(int)a].used.load(); }
size_t memCap(App a) { return CAPS[(int)a]; }

// --- the freshness cache --------------------------------------------------------------------

namespace {

// Validators and freshness for the cached requests, in RAM: losing them at a restart costs one
// full answer per URL. Keyed by a hash of the URL; a collision costs one wrong conditional
// request, which the server answers in full.
struct CacheEntry {
    uint32_t key = 0;
    int64_t  freshUntilUs = 0;
    int64_t  lastUseUs = 0;
    char     etag[64] = "";
    char     lastModified[40] = "";
};
constexpr int CACHE_N = 24;
CacheEntry s_cache[CACHE_N];
constexpr int64_t MAX_FRESH_S = 24 * 3600;   // trust no server's freshness further than a day

uint32_t hash_url(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h ? h : 1;
}

CacheEntry *cache_find(uint32_t key) {
    for (CacheEntry &e : s_cache) if (e.key == key) return &e;
    return nullptr;
}

CacheEntry *cache_slot(uint32_t key) {
    if (CacheEntry *e = cache_find(key)) return e;
    CacheEntry *oldest = &s_cache[0];
    for (CacheEntry &e : s_cache) {
        if (!e.key) { oldest = &e; break; }
        if (e.lastUseUs < oldest->lastUseUs) oldest = &e;
    }
    *oldest = CacheEntry{};
    oldest->key = key;
    return oldest;
}

// "Thu, 22 Oct 2026 17:28:00 GMT" to seconds since 1970, or -1. Expires and Date are compared
// with each other, so the Orb's own clock (which may not be set yet) never enters into it.
int64_t parse_http_date(const char *s) {
    int d, y, hh, mm, ss;
    char mon[4];
    if (!s || sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return -1;
    static const char *M = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *at = strstr(M, mon);
    if (!at || (at - M) % 3) return -1;
    int m = (int)(at - M) / 3 + 1;
    // Days from 1970-01-01 to y-m-d (Howard Hinnant's days_from_civil).
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

}  // namespace

// --- the client task ------------------------------------------------------------------------

struct Client {
    Request      req;
    TaskHandle_t waiter = nullptr;
    int          result = 0;
    // Headers worth keeping, captured as they arrive.
    char  etag[64] = "", lastModified[40] = "", expires[40] = "", date[40] = "";
    int   maxAge = -1;
    bool  noStore = false;

    static esp_err_t on_event(esp_http_client_event_t *ev);
    void run();
    void remember(uint32_t key);
};

namespace {
QueueHandle_t s_queue = nullptr;
TaskHandle_t  s_task = nullptr;

void copy_header(char *dst, size_t n, const char *v) {
    snprintf(dst, n, "%s", v ? v : "");
}
}  // namespace

esp_err_t Client::on_event(esp_http_client_event_t *ev) {
    if (ev->event_id != HTTP_EVENT_ON_HEADER || !ev->user_data) return ESP_OK;
    Client *c = (Client *)ev->user_data;
    const char *k = ev->header_key, *v = ev->header_value;
    if (!k || !v) return ESP_OK;
    if      (!strcasecmp(k, "ETag"))          copy_header(c->etag, sizeof(c->etag), v);
    else if (!strcasecmp(k, "Last-Modified")) copy_header(c->lastModified, sizeof(c->lastModified), v);
    else if (!strcasecmp(k, "Expires"))       copy_header(c->expires, sizeof(c->expires), v);
    else if (!strcasecmp(k, "Date"))          copy_header(c->date, sizeof(c->date), v);
    else if (!strcasecmp(k, "Cache-Control")) {
        if (strcasestr(v, "no-store") || strcasestr(v, "no-cache")) c->noStore = true;
        const char *ma = strcasestr(v, "max-age=");
        if (ma) c->maxAge = atoi(ma + 8);
    }
    return ESP_OK;
}

// Keep what this answer said about itself, for the next request of the same URL.
void Client::remember(uint32_t key) {
    CacheEntry *e = cache_slot(key);
    const int64_t now = esp_timer_get_time();
    e->lastUseUs = now;
    if (etag[0])         copy_header(e->etag, sizeof(e->etag), etag);
    if (lastModified[0]) copy_header(e->lastModified, sizeof(e->lastModified), lastModified);
    int64_t freshS = 0;
    if (noStore)          freshS = 0;
    else if (maxAge >= 0) freshS = maxAge;
    else {
        const int64_t ex = parse_http_date(expires), dt = parse_http_date(date);
        if (ex > 0 && dt > 0 && ex > dt) freshS = ex - dt;
    }
    if (freshS > MAX_FRESH_S) freshS = MAX_FRESH_S;
    e->freshUntilUs = now + freshS * 1000000LL;
}

void Client::run() {
    const int64_t started = esp_timer_get_time();
    const uint32_t key = req.cached ? hash_url(req.url) : 0;
    CacheEntry *entry = key ? cache_find(key) : nullptr;
    if (entry && started < entry->freshUntilUs) {
        entry->lastUseUs = started;
        result = FRESH;
        return;
    }
    if (!orb_wifi::connected()) { result = ERR_OFFLINE; return; }

    esp_http_client_config_t cfg = {};
    cfg.url               = req.url;
    cfg.user_agent        = ORB_USER_AGENT;
    cfg.method            = req.postBody ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    cfg.timeout_ms        = req.timeoutMs;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = false;   // one request, one connection, then let it all go
    cfg.buffer_size       = 2048;
    cfg.event_handler     = on_event;
    cfg.user_data         = this;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { result = ERR_MEMORY; return; }
    if (req.accept) esp_http_client_set_header(h, "Accept", req.accept);
    esp_http_client_set_header(h, "Accept-Encoding", "identity");   // nothing here inflates gzip
    if (entry && entry->etag[0])         esp_http_client_set_header(h, "If-None-Match", entry->etag);
    if (entry && entry->lastModified[0]) esp_http_client_set_header(h, "If-Modified-Since", entry->lastModified);
    const int postLen = req.postBody ? (int)strlen(req.postBody) : 0;
    if (req.postBody) esp_http_client_set_header(h, "Content-Type", req.contentType);

    int status = ERR_CONNECT;
    size_t bytes = 0;
    if (esp_http_client_open(h, postLen) == ESP_OK &&
        (postLen == 0 || esp_http_client_write(h, req.postBody, postLen) == postLen)) {
        const int64_t declared = esp_http_client_fetch_headers(h);   // -1/0 when chunked
        status = esp_http_client_get_status_code(h);
        if (status <= 0) {
            status = ERR_CONNECT;
        } else if (status == 304) {
            if (key) remember(key);
        } else if (status == 200) {
            if (declared > 0 && (size_t)declared > req.maxBytes) {
                printf("[net] %s: %lld bytes is over the %u limit\n", req.url, (long long)declared,
                       (unsigned)req.maxBytes);
                status = ERR_BODY;
            } else {
                Body body;
                body._client = h;
                body._max = req.maxBytes;
                bool ok;
                if (req.handler) {
                    ok = req.handler(body, req.ctx);
                } else {
                    char sink[256];
                    while (body.readChunk(sink, sizeof(sink))) {}
                    ok = true;
                }
                bytes = body.bytesRead();
                if (!ok || body.failed()) status = ERR_BODY;
                else if (key) remember(key);
            }
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    result = status;

    // One line per request: who, what, how it went, the internal RAM a handshake needs, and
    // the least stack this task has had left (its size in begin() is checked against it).
    char host[64] = "";
    const char *p = strstr(req.url, "://");
    p = p ? p + 3 : req.url;
    size_t i = 0;
    while (p[i] && p[i] != '/' && i < sizeof(host) - 1) { host[i] = p[i]; ++i; }
    host[i] = 0;
    printf("[net] %s %s %s -> %d (%u B, %lld ms) internal_largest=%u stack_free=%u\n", NAMES[(int)req.app],
           req.postBody ? "POST" : "GET", host, status, (unsigned)bytes,
           (long long)((esp_timer_get_time() - started) / 1000),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

// --- Body -----------------------------------------------------------------------------------

bool Body::refill() {
    if (_failed || _ended) return false;
    _pos = _len = 0;
    if (_total >= _max) {   // more is coming, and more is more than this request allows
        _failed = true;
        return false;
    }
    size_t want = sizeof(_buf);
    if (want > _max - _total) want = _max - _total;
    esp_http_client_handle_t h = (esp_http_client_handle_t)_client;
    const int n = esp_http_client_read(h, _buf, (int)want);
    if (n < 0) { _failed = true; return false; }   // the connection failed mid-body
    if (n == 0) {
        // Zero is "nothing more": the end of a complete answer, or a stall the client gave up
        // on. Only the first is a body.
        _ended = true;
        if (!esp_http_client_is_complete_data_received(h)) _failed = true;
        return false;
    }
    _len = (size_t)n;
    _total += _len;
    return true;
}

int Body::read() {
    if (_pos >= _len && !refill()) return -1;
    return (uint8_t)_buf[_pos++];
}

size_t Body::readBytes(char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (_pos >= _len && !refill()) break;
        size_t take = _len - _pos;
        if (take > n - got) take = n - got;
        memcpy(buf + got, _buf + _pos, take);
        _pos += take;
        got += take;
    }
    return got;
}

size_t Body::readChunk(char *buf, size_t n) { return readBytes(buf, n); }

// --- the queue ------------------------------------------------------------------------------

namespace {

void client_task(void *) {
    for (;;) {
        Client *c = nullptr;
        if (xQueueReceive(s_queue, &c, portMAX_DELAY) != pdTRUE || !c) continue;
        c->run();
        xTaskNotifyGive(c->waiter);
    }
}

}  // namespace

void begin() {
    if (s_queue) return;
    s_queue = xQueueCreate(8, sizeof(Client *));
    // Core 0 with the rest of the network. The TLS handshake runs on this stack, and the
    // handlers' parsers after it. Every request logs the stack's low-water mark (stack_free).
    xTaskCreatePinnedToCore(client_task, "net_client", 10240, nullptr, 2, &s_task, 0);
}

int fetch(const Request &req) {
    if (!s_queue || !req.url) return ERR_MISUSE;
    if (xTaskGetCurrentTaskHandle() == s_task) {
        printf("[net] fetch() from a handler would wait on itself; refused\n");
        return ERR_MISUSE;
    }
    // A fresh cached answer needs no network, so only an uncached request is refused offline.
    if (!req.cached && !orb_wifi::connected()) return ERR_OFFLINE;
    Client *c = new (std::nothrow) Client;
    if (!c) return ERR_MEMORY;
    c->req = req;
    c->waiter = xTaskGetCurrentTaskHandle();
    xTaskNotifyStateClear(nullptr);
    xQueueSend(s_queue, &c, portMAX_DELAY);
    // Every request ends on the client's own timeouts, so this wait does too.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const int r = c->result;
    delete c;
    return r;
}

}  // namespace orb_net
