// The Orb OS — one HTTPS GET, on ESP-IDF's esp_http_client.
//
// Certificates are verified against ESP-IDF's root bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE),
// as REWRITE_PLAN.md asks. The Arduino build skipped verification (ADSB_HTTPS_INSECURE).
// The body lands in PSRAM, whole: a feed response is tens of KB, PSRAM has megabytes, and
// keeping it off the internal heap is what keeps a TLS handshake's contiguous block there.
//
// Stage 4 of the plan replaces this with one queued HTTPS client task. Until then, call it
// from the network task only.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace orb_http {

struct Request {
    const char *url;
    const char *userAgent;
    const char *accept   = nullptr;   // Accept header, if any
    int connectTimeoutMs = 5000;
    int readTimeoutMs    = 20000;
    size_t maxBytes      = 256 * 1024;   // a body larger than this is refused, not truncated
};

// The HTTP status (200, 404, ...), or a negative number when no answer arrived: -1 could not
// connect or the TLS handshake failed, -2 the body was cut short or too large, -3 no memory.
// On 200 the body is in *body (PSRAM, NUL-terminated, not counted in *len); release it with
// orb_http::release(). On anything else *body is null.
int get(const Request &req, uint8_t **body, size_t *len);
void release(uint8_t *body);

}  // namespace orb_http
