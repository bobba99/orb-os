// The Orb OS — one HTTPS GET. See orb_http.h.
#include "orb_http.h"

#include <string.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "http";

void orb_http::release(uint8_t *body) { if (body) heap_caps_free(body); }

int orb_http::get(const Request &req, uint8_t **body, size_t *len) {
    *body = nullptr;
    *len = 0;

    esp_http_client_config_t cfg = {};
    cfg.url               = req.url;
    cfg.user_agent        = req.userAgent;
    cfg.method            = HTTP_METHOD_GET;
    // esp_http_client has one timeout for every network wait; the read budget is the longer
    // of the two and covers the connect, which ESP-IDF bounds on its own.
    cfg.timeout_ms        = req.readTimeoutMs > req.connectTimeoutMs ? req.readTimeoutMs
                                                                     : req.connectTimeoutMs;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = false;    // one request, one connection, then let it all go
    cfg.buffer_size       = 2048;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -3;
    if (req.accept) esp_http_client_set_header(c, "Accept", req.accept);
    // The feeds answer gzip if allowed to; nothing here inflates it.
    esp_http_client_set_header(c, "Accept-Encoding", "identity");

    int status = -1;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        esp_http_client_cleanup(c);
        return -1;
    }
    const int64_t declared = esp_http_client_fetch_headers(c);   // -1/0 when chunked
    status = esp_http_client_get_status_code(c);
    if (status != 200) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return status > 0 ? status : -1;
    }
    if (declared > 0 && (size_t)declared > req.maxBytes) {
        ESP_LOGW(TAG, "%s: %lld bytes is over the %u limit", req.url, (long long)declared,
                 (unsigned)req.maxBytes);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return -2;
    }

    // Grown in PSRAM as it arrives: chunked replies (adsb.lol's) declare no length.
    size_t cap = declared > 0 ? (size_t)declared + 1 : 32 * 1024;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { esp_http_client_close(c); esp_http_client_cleanup(c); return -3; }
    size_t got = 0;
    int result = 200;
    for (;;) {
        if (got + 1 >= cap) {
            if (cap >= req.maxBytes) { result = -2; break; }
            size_t next = cap * 2 > req.maxBytes ? req.maxBytes : cap * 2;
            uint8_t *grown = (uint8_t *)heap_caps_realloc(buf, next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) { result = -3; break; }
            buf = grown; cap = next;
        }
        const int n = esp_http_client_read(c, (char *)buf + got, (int)(cap - 1 - got));
        if (n < 0) { result = -2; break; }      // the connection failed mid-body
        if (n == 0) {
            // Zero is "nothing more": the end of a complete response, or a stall the
            // client gave up on. Only the first is a body.
            if (!esp_http_client_is_complete_data_received(c)) result = -2;
            break;
        }
        got += (size_t)n;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (result != 200) { heap_caps_free(buf); return result; }
    buf[got] = 0;
    *body = buf;
    *len = got;
    return 200;
}
