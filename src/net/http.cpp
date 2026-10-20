// The Orb OS — plain-HTTP requests made from the device's top level.
//
// The sequence below is the one ip_lookup_location() and host_geocode() each carried in
// location.cpp, moved here unchanged: same timeouts, same order, same early returns.
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "net/http.h"

bool http_get_text(const char *url, String &body) {
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(6000);
    if (!http.begin(client, url)) return false;
    if (http.GET() != 200) { http.end(); return false; }
    body = http.getString();
    http.end();
    return true;
}
