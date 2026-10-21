// The Orb OS — WiFi on ESP-IDF, the stage 2 minimum. See orb_wifi.h.
#include "orb_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

static constexpr uint32_t BACKOFF_FIRST_MS = 1000;
static constexpr uint32_t BACKOFF_MAX_MS   = 30000;

static volatile bool s_connected = false;
static volatile bool s_synced = false;
static bool s_haveNetwork = false;
static bool s_sntpStarted = false;
static uint32_t s_backoffMs = BACKOFF_FIRST_MS;
static esp_timer_handle_t s_retry = nullptr;
static char s_ssid[33] = "";

static void retry_cb(void *) { esp_wifi_connect(); }

static void on_sntp_sync(struct timeval *) {
    if (!s_synced) printf("[wifi] clock set by NTP\n");
    s_synced = true;
}

static void start_sntp() {
    if (s_sntpStarted) return;
    s_sntpStarted = true;
    // The same two servers configTzTime() was given on Arduino. esp_netif_sntp takes the
    // TCP/IP core lock itself, which is the lock the Arduino build's crash on a flapping link
    // was missing (sys_untimeout asserting from a DNS callback).
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.nist.gov"));
    cfg.sync_cb = on_sntp_sync;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) ESP_LOGW(TAG, "SNTP did not start");
}

static void on_event(void *, esp_event_base_t base, int32_t id, void *data) {
    if (false) {
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const bool was = s_connected;
        s_connected = false;
        const auto *d = (const wifi_event_sta_disconnected_t *)data;
        // Said every time, with the driver's reason: the Arduino build's silence about
        // drops is why four hours of "No WiFi" went unexplained there (2026-08-23).
        printf("[wifi] %s '%s' (reason %d); retrying in %lu s\n",
               was ? "connection LOST from" : "could not join", s_ssid, d ? d->reason : -1,
               (unsigned long)(s_backoffMs / 1000));
        esp_timer_start_once(s_retry, (uint64_t)s_backoffMs * 1000);
        s_backoffMs = s_backoffMs * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoffMs * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto *e = (const ip_event_got_ip_t *)data;
        s_connected = true;
        s_backoffMs = BACKOFF_FIRST_MS;
        // Modem sleep off, as WiFi.setSleep(false) did: on a mains-powered desk instrument it
        // only adds latency and makes the signal reading bounce.
        esp_wifi_set_ps(WIFI_PS_NONE);
        printf("[wifi] up, IP " IPSTR "\n", IP2STR(&e->ip_info.ip));
        start_sntp();
    }
}

bool orb_wifi::begin() {
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "network stack: %s", esp_err_to_name(err));
        return false;
    }
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, "theorb");

    // Init reads the stored network from the driver's NVS namespace (orb_prefs::begin() has
    // already initialised NVS, and never erases it).
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return false;
    }
    // ...and from here on the driver writes nothing back. Every call below that would
    // otherwise persist (the mode, the power-save setting) now stays in RAM, so the network
    // the owner saved under the Arduino firmware is read and can never be changed by this one.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);
    const esp_timer_create_args_t t = { .callback = retry_cb, .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK, .name = "wifi_retry",
                                        .skip_unhandled_events = true };
    esp_timer_create(&t, &s_retry);

    esp_wifi_set_mode(WIFI_MODE_STA);
#ifdef CONFIG_ORB_WIFI_COUNTRY
    if (strcmp(CONFIG_ORB_WIFI_COUNTRY, "01") != 0 &&
        esp_wifi_set_country_code(CONFIG_ORB_WIFI_COUNTRY, true) != ESP_OK)
        ESP_LOGW(TAG, "country code '%s' was not accepted", CONFIG_ORB_WIFI_COUNTRY);
#endif
    wifi_config_t stored = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &stored) == ESP_OK)
        snprintf(s_ssid, sizeof(s_ssid), "%s", (const char *)stored.sta.ssid);
    printf("[wifi] stored network: %s\n", s_ssid[0] ? s_ssid : "(none)");
#ifdef CONFIG_ORB_DEV_WIFI_SSID
    if (CONFIG_ORB_DEV_WIFI_SSID[0]) {
        // A development build's network, joined instead of the stored one. Storage is RAM
        // since the call above, so this replaces nothing the owner saved.
        wifi_config_t dev = {};
        snprintf((char *)dev.sta.ssid, sizeof(dev.sta.ssid), "%s", CONFIG_ORB_DEV_WIFI_SSID);
        snprintf((char *)dev.sta.password, sizeof(dev.sta.password), "%s", CONFIG_ORB_DEV_WIFI_PASS);
        dev.sta.threshold.authmode = CONFIG_ORB_DEV_WIFI_PASS[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        if (esp_wifi_set_config(WIFI_IF_STA, &dev) == ESP_OK) {
            stored = dev;
            snprintf(s_ssid, sizeof(s_ssid), "%s", CONFIG_ORB_DEV_WIFI_SSID);
            printf("[wifi] development build: joining '%s' instead (RAM only, nothing saved)\n", s_ssid);
        }
    }
#endif
    s_haveNetwork = s_ssid[0] != 0;
    if (s_haveNetwork) {
        // What the stored entry asks of the radio. A BSSID or channel lock, or a security
        // threshold the router does not meet, each make "no access point found" (reason 201)
        // out of a network that is plainly there.
        const wifi_sta_config_t &c = stored.sta;
        wifi_country_t cc = {};
        esp_wifi_get_country(&cc);
        printf("[wifi] stored entry: bssid_set=%d channel=%d threshold_auth=%d pmf_required=%d "
               "scan_method=%d; country %.2s ch %d-%d\n",
               (int)c.bssid_set, (int)c.channel, (int)c.threshold.authmode, (int)c.pmf_cfg.required,
               (int)c.scan_method, cc.cc, (int)cc.schan, (int)(cc.schan + cc.nchan - 1));
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return false;
    }
    if (s_haveNetwork) {
        // One look around before the first attempt, so a failure to join says whether the
        // network was seen at all, and where. About two seconds, once, at boot.
        wifi_scan_config_t scan = {};
        scan.show_hidden = true;
        if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            wifi_ap_record_t *aps = n ? (wifi_ap_record_t *)calloc(n, sizeof(wifi_ap_record_t)) : nullptr;
            if (aps && esp_wifi_scan_get_ap_records(&n, aps) == ESP_OK) {
                int seen = 0;
                for (int i = 0; i < n; ++i) {
                    if (strcmp((const char *)aps[i].ssid, s_ssid) != 0) continue;
                    ++seen;
                    printf("[wifi] scan: '%s' on channel %d, %d dBm, auth %d, bssid "
                           "%02x:%02x:%02x:%02x:%02x:%02x\n", s_ssid, aps[i].primary, aps[i].rssi,
                           (int)aps[i].authmode, aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                           aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
                }
                if (!seen) {
                    printf("[wifi] scan: '%s' not among the %u networks seen:\n", s_ssid, (unsigned)n);
                    for (int i = 0; i < n; ++i)
                        printf("[wifi]   '%s'  channel %d, %d dBm, auth %d\n", (const char *)aps[i].ssid,
                               aps[i].primary, aps[i].rssi, (int)aps[i].authmode);
                }
            }
            free(aps);
        }
        esp_wifi_connect();
    }
    return true;
}

bool orb_wifi::connected() { return s_connected; }

int orb_wifi::rssi() {
    if (!s_connected) return -127;
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : -127;
}

void orb_wifi::savedSsid(char *out, size_t n) { if (n) snprintf(out, n, "%s", s_ssid); }

bool orb_wifi::timeSynced() { return s_synced; }
