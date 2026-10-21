// The Orb OS — plain-HTTP requests made from the device's top level. See net/http.cpp.
//
// The feeds have their own clients (adsb_client, weather_client, net_fetch for large
// bodies into PSRAM); this is the small-text GET that location.cpp used to spell out
// twice. Stage 4 of REWRITE_PLAN.md replaces all of them with one HTTPS client task.
#pragma once
#include <Arduino.h>

// GET a small text body, such as a JSON answer of a few KB. 4 s to connect, 6 s for the
// whole reply. False on any failure, a non-200 status included, and then `body` is untouched.
bool http_get_text(const char *url, String &body);
