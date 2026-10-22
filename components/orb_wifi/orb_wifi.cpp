// The Orb OS — WiFi on ESP-IDF: the state machine. See orb_wifi.h for the states and the two
// promises this file keeps.
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
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "orb_prefs.h"
#include "portal.h"

static const char *TAG = "wifi";

namespace {

constexpr uint32_t BACKOFF_FIRST_MS = 1000;
constexpr uint32_t BACKOFF_MAX_MS   = 30000;
constexpr uint32_t ATTEMPT_MS       = 20000;   // association + DHCP, before it counts as failed
constexpr const char *LIST_KEY      = "wifiNets";
constexpr uint8_t LIST_VERSION      = 1;
// The setup access point. Open while it is up: see portal.cpp for why, and what replaces it.
constexpr const char *AP_SSID       = "The Orb Setup";
constexpr uint32_t PROVISION_ROUND_MS = 30000;   // saved networks retried this often while it is up
constexpr uint32_t PORTAL_LINGER_MS   = 60000;   // kept up after a join, so the phone sees "joined"
#ifdef CONFIG_ORB_WIFI_SETUP_AFTER_S
constexpr int64_t SETUP_AFTER_US = (int64_t)CONFIG_ORB_WIFI_SETUP_AFTER_S * 1000000;
#else
constexpr int64_t SETUP_AFTER_US = 300LL * 1000000;
#endif

// --- the saved list, as stored -----------------------------------------------------------
struct Net {
    char ssid[33];
    char pass[65];   // plain, as the WiFi driver itself stores it in its own namespace
};
struct SavedList {
    uint8_t version;
    uint8_t count;
    uint8_t reserved[2];
    Net     nets[orb_wifi::MAX_SAVED];
};

// --- what reaches the manager task ---------------------------------------------------------
enum EvType : uint8_t { EV_START, EV_DISCONNECTED, EV_GOT_IP, EV_TIMER, EV_TRY, EV_FORGET, EV_SCAN,
                       EV_PORTAL_CLOSE };
struct Ev {
    EvType   type;
    uint8_t  reason;      // EV_DISCONNECTED
    uint32_t gen;         // EV_TIMER: which arming this was, so a stale one is ignored;
                          // EV_DISCONNECTED: 1 when it is the manager's own disconnect
    uint32_t ip;          // EV_GOT_IP
    char     ssid[33];    // EV_DISCONNECTED: the network the driver says it left
};

// A candidate for this round. The dev network rides here too, flagged so it is never saved.
struct Cand {
    Net    net;
    int8_t rssi;          // -127 when not seen in the scan
    bool   dev;
};

QueueHandle_t      s_q = nullptr;
SemaphoreHandle_t  s_lock = nullptr;      // guards s_list, s_current, the scan results
esp_timer_handle_t s_timer = nullptr;
volatile uint32_t  s_timerGen = 0;
volatile uint32_t  s_armedGen = 0;

SavedList s_list = {};
bool      s_listPersisted = false;        // false: NVS unusable, so the list lives in RAM

volatile orb_wifi::State     s_state = orb_wifi::State::Idle;
volatile bool                s_synced = false;
volatile orb_wifi::TryResult s_try = orb_wifi::TryResult::None;
char  s_current[33] = "";
Net   s_tryNet = {};
bool  s_trying = false;
bool  s_haveDev = false;
Net   s_dev = {};

Cand  s_cands[orb_wifi::MAX_SAVED + 1];
int   s_candCount = 0, s_candIdx = 0;
uint32_t s_backoffMs = BACKOFF_FIRST_MS;
bool  s_sntpStarted = false;
volatile bool s_expectDisconnect = false; // our own esp_wifi_disconnect(), not a lost link
bool  s_portalOn = false;
bool  s_scanPending = false;              // a picker asked to look again while the radio was joining
int64_t s_offlineSinceUs = 0;             // when the Orb was last connected (or booted); 0 while connected
esp_timer_handle_t s_portalTimer = nullptr;

// Picker scan results.
volatile int s_scanState = -2;            // -1 running, -2 failed/none, >=0 count
struct ScanEntry { char ssid[33]; int8_t rssi; bool open; };
ScanEntry s_scan[24];

// --- helpers ------------------------------------------------------------------------------


// A bounded, always-terminated copy. snprintf("%s") would do, but GCC cannot see the source
// bound through an array index and refuses to build.
void copy_str(char *dst, size_t dstSize, const char *src) {
    if (!dstSize) return;
    const size_t n = strnlen(src ? src : "", dstSize - 1);
    memcpy(dst, src ? src : "", n);
    dst[n] = 0;
}

void post(EvType t, uint8_t reason = 0, uint32_t gen = 0, uint32_t ip = 0, const char *ssid = nullptr) {
    Ev e = {};
    e.type = t; e.reason = reason; e.gen = gen; e.ip = ip;
    if (ssid) copy_str(e.ssid, sizeof(e.ssid), ssid);
    if (s_q) xQueueSend(s_q, &e, 0);
}

// One timer for everything the manager waits on (an attempt's 20 s, a backoff). Each arming
// gets a generation, and an expiry from an older arming is dropped, so a timer that fires
// just as it is replaced cannot end the wrong wait.
void timer_cb(void *) { post(EV_TIMER, 0, s_armedGen); }

void arm(uint32_t ms) {
    esp_timer_stop(s_timer);
    s_timerGen = s_timerGen + 1;
    s_armedGen = s_timerGen;
    esp_timer_start_once(s_timer, (uint64_t)ms * 1000);
}

void disarm() {
    esp_timer_stop(s_timer);
    s_timerGen = s_timerGen + 1;
}

void set_state(orb_wifi::State st) {
    if (s_state != st) printf("[wifi] %s\n", orb_wifi::stateName(st));
    s_state = st;
}

void save_list() {
    if (s_listPersisted) orb_prefs::putBlob(LIST_KEY, &s_list, sizeof(s_list));
}

int find_saved(const char *ssid) {
    for (int i = 0; i < s_list.count; ++i) if (!strcmp(s_list.nets[i].ssid, ssid)) return i;
    return -1;
}

// Move (or add) a network to the front: the last network that worked is tried first.
// Written only when the order or the password actually changes, so a stable home network
// costs no NVS writes at all.
void promote(const Net &n) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int at = find_saved(n.ssid);
    const bool changed = !(at == 0 && !strcmp(s_list.nets[0].pass, n.pass));
    if (changed) {
        // Shift everything above the old slot down one. A new network when the list is full
        // pushes the last (least recently working) one off the end.
        const int from = at >= 0 ? at : (s_list.count < orb_wifi::MAX_SAVED ? s_list.count
                                                                            : orb_wifi::MAX_SAVED - 1);
        memmove(&s_list.nets[1], &s_list.nets[0], sizeof(Net) * from);
        s_list.nets[0] = n;
        if (at < 0 && s_list.count < orb_wifi::MAX_SAVED) ++s_list.count;
    }
    const int count = s_list.count;
    xSemaphoreGive(s_lock);
    if (changed) {
        save_list();
        printf("[wifi] saved networks: '%s' first, %d in all\n", n.ssid, count);
    }
}

void on_sntp_sync(struct timeval *) {
    if (!s_synced) printf("[wifi] clock set by NTP\n");
    s_synced = true;
}

void start_sntp() {
    if (s_sntpStarted) return;
    s_sntpStarted = true;
    // The same two servers configTzTime() was given on Arduino. esp_netif_sntp takes the TCP/IP
    // core lock itself, which the Arduino build's crash on a flapping link was missing.
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.nist.gov"));
    cfg.sync_cb = on_sntp_sync;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) ESP_LOGW(TAG, "SNTP did not start");
}

// A blocking scan, run only from the manager task, so a round and a picker never fight over
// the radio. Fills the picker's results too. Returns the record count, -1 on failure; the
// caller frees *out.
int scan_now(wifi_ap_record_t **out) {
    *out = nullptr;
    wifi_scan_config_t sc = {};
    s_scanState = -1;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) { s_scanState = -2; return -1; }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *aps = n ? (wifi_ap_record_t *)calloc(n, sizeof(wifi_ap_record_t)) : nullptr;
    if (n && (!aps || esp_wifi_scan_get_ap_records(&n, aps) != ESP_OK)) {
        free(aps);
        s_scanState = -2;
        return -1;
    }
    // Picker results: one row per name, at its strongest access point's signal.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int k = 0;
    for (int i = 0; i < n; ++i) {
        const char *ss = (const char *)aps[i].ssid;
        if (!ss[0]) continue;
        int j = 0;
        while (j < k && strcmp(s_scan[j].ssid, ss)) ++j;
        if (j < k) { if (aps[i].rssi > s_scan[j].rssi) s_scan[j].rssi = aps[i].rssi; continue; }
        if (k >= (int)(sizeof(s_scan) / sizeof(s_scan[0]))) continue;
        copy_str(s_scan[k].ssid, sizeof(s_scan[k].ssid), ss);
        s_scan[k].rssi = aps[i].rssi;
        s_scan[k].open = aps[i].authmode == WIFI_AUTH_OPEN;
        ++k;
    }
    xSemaphoreGive(s_lock);
    s_scanState = k;
    *out = aps;
    return n;
}

int8_t seen_rssi(const wifi_ap_record_t *aps, int n, const char *ssid) {
    int8_t best = -127;
    for (int i = 0; i < n; ++i)
        if (!strcmp((const char *)aps[i].ssid, ssid) && aps[i].rssi > best) best = aps[i].rssi;
    return best;
}

// A picker's "look again" that arrived mid-attempt, run now that the radio is free.
void run_pending_scan() {
    if (!s_scanPending) return;
    s_scanPending = false;
    wifi_ap_record_t *aps = nullptr;
    scan_now(&aps);
    free(aps);
}

// Join one network. The driver is on RAM storage, so this configuration is never written.
void attempt(const Net &n, const char *why) {
    wifi_config_t cfg = {};
    // The driver's fields are exactly 32 and 64 bytes and need no terminator: a 32-character
    // name fills its field. snprintf would have dropped that name's last character.
    memcpy(cfg.sta.ssid, n.ssid, strnlen(n.ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, n.pass, strnlen(n.pass, sizeof(cfg.sta.password)));
    cfg.sta.threshold.authmode = n.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;   // several access points, one name: the strongest
    s_expectDisconnect = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));                     // let that disconnect's event go by unseen
    s_expectDisconnect = false;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_str(s_current, sizeof(s_current), n.ssid);
    xSemaphoreGive(s_lock);
    printf("[wifi] trying '%s' (%s)\n", n.ssid, why);
    set_state(orb_wifi::State::Connecting);
    esp_wifi_connect();
    arm(ATTEMPT_MS);
}

// The setup access point, alongside the station: APSTA, so the station side keeps trying the
// saved networks the whole time. The Arduino firmware's portal turned the station off, and an
// Orb with a perfectly good saved network then sat in setup until somebody pulled the plug.
void open_portal(const char *why) {
    if (s_portalOn) return;
    esp_timer_stop(s_portalTimer);
    esp_wifi_set_mode(WIFI_MODE_APSTA);               // RAM storage: never written
    wifi_config_t ap = {};
    memcpy(ap.ap.ssid, AP_SSID, strlen(AP_SSID));
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_portalOn = portal::start();
    printf("[wifi] '%s' is up (%s)\n", AP_SSID, why);
}

void close_portal() {
    if (!s_portalOn) return;
    portal::stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_portalOn = false;
    printf("[wifi] '%s' closed\n", AP_SSID);
}

// Start a round: scan, then line up the candidates. Visible saved networks strongest first,
// then the ones the scan did not see.
void start_round() {
    wifi_ap_record_t *aps = nullptr;
    const int n = scan_now(&aps);
    s_candCount = 0;
    s_candIdx = 0;
    auto add = [&](const Net &net, bool dev) {
        if (s_candCount >= (int)(sizeof(s_cands) / sizeof(s_cands[0]))) return;
        for (int i = 0; i < s_candCount; ++i) if (!strcmp(s_cands[i].net.ssid, net.ssid)) return;
        s_cands[s_candCount++] = { net, n > 0 ? seen_rssi(aps, n, net.ssid) : (int8_t)-127, dev };
    };
    if (s_haveDev) add(s_dev, true);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_list.count; ++i) add(s_list.nets[i], false);
    xSemaphoreGive(s_lock);
    free(aps);
    // Seen ones first, strongest first; the unseen keep the saved order behind them. Stable,
    // so two networks at the same strength keep their priority.
    for (int i = 1; i < s_candCount; ++i)
        for (int j = i; j > 0 && s_cands[j].rssi > s_cands[j - 1].rssi; --j) {
            Cand t = s_cands[j]; s_cands[j] = s_cands[j - 1]; s_cands[j - 1] = t;
        }
    if (s_candCount == 0) {
        // Nothing saved and no development network: there is nothing to try, so the setup
        // access point opens at once and the Orb waits on it. No timer: a network arrives
        // through tryNetwork() from the page, not from a retry.
        open_portal("no saved network");
        set_state(orb_wifi::State::Provisioning);
        return;
    }
    printf("[wifi] round:");
    for (int i = 0; i < s_candCount; ++i) {
        if (s_cands[i].rssi > -127) printf("%s '%s' %d dBm", i ? "," : "", s_cands[i].net.ssid, s_cands[i].rssi);
        else                        printf("%s '%s' not seen", i ? "," : "", s_cands[i].net.ssid);
    }
    printf("\n");
    char why[32];
    snprintf(why, sizeof(why), "1 of %d", s_candCount);
    attempt(s_cands[0].net, why);
}

// The current attempt failed: the next candidate, or a pause when the round is spent.
void next_candidate() {
    ++s_candIdx;
    if (s_candIdx < s_candCount) {
        char why[32];
        snprintf(why, sizeof(why), "%d of %d", s_candIdx + 1, s_candCount);
        attempt(s_cands[s_candIdx].net, why);
        return;
    }
    s_expectDisconnect = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));
    s_expectDisconnect = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_current[0] = 0;
    xSemaphoreGive(s_lock);
    // Long enough offline and the setup access point opens; the saved networks are still tried,
    // more slowly, for as long as it is up.
    run_pending_scan();
    if (!s_portalOn && esp_timer_get_time() - s_offlineSinceUs >= SETUP_AFTER_US)
        open_portal("no saved network has answered for a while");
    if (s_portalOn) {
        printf("[wifi] no saved network answered; trying again in %lu s\n",
               (unsigned long)(PROVISION_ROUND_MS / 1000));
        set_state(orb_wifi::State::Provisioning);
        arm(PROVISION_ROUND_MS);
        return;
    }
    printf("[wifi] no saved network answered; next round in %lu s\n", (unsigned long)(s_backoffMs / 1000));
    set_state(orb_wifi::State::Backoff);
    arm(s_backoffMs);
    s_backoffMs = s_backoffMs * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoffMs * 2;
}

void try_failed(const char *why) {
    printf("[wifi] '%s' did not join (%s); back to the saved networks\n", s_tryNet.ssid, why);
    s_trying = false;
    s_try = orb_wifi::TryResult::Failed;
    s_backoffMs = BACKOFF_FIRST_MS;
    start_round();
}

void manager(void *) {
    Ev e;
    for (;;) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) != pdTRUE) continue;
        switch (e.type) {
        case EV_START:
            start_round();
            break;

        case EV_GOT_IP: {
            disarm();
            s_backoffMs = BACKOFF_FIRST_MS;
            esp_wifi_set_ps(WIFI_PS_NONE);   // as WiFi.setSleep(false) did
            char ssid[33];
            xSemaphoreTake(s_lock, portMAX_DELAY);
            copy_str(ssid, sizeof(ssid), s_current);
            xSemaphoreGive(s_lock);
            esp_ip4_addr_t ip = { e.ip };
            printf("[wifi] joined '%s', IP " IPSTR "\n", ssid, IP2STR(&ip));
            if (s_trying) {
                // It produced an address, so now it is true, so now it is written.
                promote(s_tryNet);
                s_trying = false;
                s_try = orb_wifi::TryResult::Joined;
            } else if (s_candIdx < s_candCount && !s_cands[s_candIdx].dev) {
                promote(s_cands[s_candIdx].net);
            }
            set_state(orb_wifi::State::Connected);
            s_offlineSinceUs = 0;
            run_pending_scan();
            start_sntp();
            // Joined with the setup network up: leave it up a minute, so the phone on the page
            // reads "joined" from the Orb rather than losing it mid-sentence.
            if (s_portalOn) {
                esp_timer_stop(s_portalTimer);
                esp_timer_start_once(s_portalTimer, (uint64_t)PORTAL_LINGER_MS * 1000);
            }
            break;
        }

        case EV_DISCONNECTED: {
            // What a disconnect means depends on what the manager is doing, not on when it
            // arrives. In BACKOFF, IDLE or PROVISIONING the radio is joining nothing, so the
            // event is the tail of the manager's own disconnect and changes nothing; above all
            // it must not touch the timer, or the backoff it is waiting on never ends (seen on
            // the Orb, 2026-10-21: one late event left the Orb in BACKOFF for good). While
            // joining or joined, an event naming another network is the tail of an earlier
            // attempt and is ignored the same way.
            // The manager's own esp_wifi_disconnect(): marked when the driver reported it (the
            // flag is long cleared by the time the queue gets here, which is how a try of the
            // network the Orb was already on failed at once, 2026-10-22), or by its reason.
            if (e.gen || e.reason == WIFI_REASON_ASSOC_LEAVE) break;
            if (s_state != orb_wifi::State::Connecting && s_state != orb_wifi::State::Connected) break;
            char cur[33];
            xSemaphoreTake(s_lock, portMAX_DELAY);
            copy_str(cur, sizeof(cur), s_current);
            xSemaphoreGive(s_lock);
            if (e.ssid[0] && strcmp(e.ssid, cur) != 0) break;
            disarm();
            if (s_trying) {
                char r[24];
                snprintf(r, sizeof(r), "reason %u", e.reason);
                try_failed(r);
            } else if (s_state == orb_wifi::State::Connected) {
                // The link went: a fresh round at once, in which the network that just dropped
                // competes on signal with the rest, rather than a wait on a timer.
                printf("[wifi] connection LOST from '%s' (reason %u)\n", s_current, e.reason);
                s_offlineSinceUs = esp_timer_get_time();
                s_backoffMs = BACKOFF_FIRST_MS;
                start_round();
            } else if (s_state == orb_wifi::State::Connecting) {
                printf("[wifi] '%s' failed (reason %u)\n", s_current, e.reason);
                next_candidate();
            }
            break;
        }

        case EV_TIMER:
            if (e.gen != s_timerGen) break;   // superseded by a later arming
            if (s_trying) {
                try_failed("no address in 20 s");
            } else if (s_state == orb_wifi::State::Connecting) {
                printf("[wifi] '%s' gave no address in 20 s\n", s_current);
                next_candidate();
            } else if (s_state == orb_wifi::State::Backoff || s_state == orb_wifi::State::Provisioning) {
                start_round();
            }
            break;

        case EV_PORTAL_CLOSE:
            // Only once the Orb is actually online; if the link went again in the meantime the
            // setup network is still the way back.
            if (s_state == orb_wifi::State::Connected) close_portal();
            break;

        case EV_TRY:
            disarm();
            s_trying = true;
            attempt(s_tryNet, "asked for");
            break;

        case EV_FORGET:
            break;   // the list was edited under the lock; nothing for the radio to do

        case EV_SCAN: {
            // The radio cannot scan while it is joining, so a request mid-attempt waits for the
            // attempt to end (run_pending_scan) rather than being answered with an empty list.
            if (s_state == orb_wifi::State::Connecting) { s_scanPending = true; break; }
            wifi_ap_record_t *aps = nullptr;
            scan_now(&aps);
            free(aps);
            break;
        }
        }
    }
}

void on_event(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        post(EV_START);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *d = (const wifi_event_sta_disconnected_t *)data;
        char ss[33] = "";
        if (d) memcpy(ss, d->ssid, d->ssid_len < 32 ? d->ssid_len : 32);   // not terminated by the driver
        // gen 1: this is the tail of the manager's own disconnect. Read here, in the event
        // task, while attempt() is still waiting; the queue is read only after it has stopped.
        post(EV_DISCONNECTED, d ? d->reason : 0, s_expectDisconnect ? 1 : 0, 0, ss);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto *g = (const ip_event_got_ip_t *)data;
        post(EV_GOT_IP, 0, 0, g->ip_info.ip.addr);
    }
}

}  // namespace

const char *orb_wifi::stateName(State s) {
    switch (s) {
    case State::Idle:         return "IDLE";
    case State::Connecting:   return "CONNECTING";
    case State::Connected:    return "CONNECTED";
    case State::Backoff:      return "BACKOFF";
    case State::Provisioning: return "PROVISIONING";
    }
    return "?";
}

bool orb_wifi::begin() {
    s_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreate(16, sizeof(Ev));
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "network stack: %s", esp_err_to_name(err));
        return false;
    }
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, "theorb");
    esp_netif_create_default_wifi_ap();   // used only while "The Orb Setup" is up (192.168.4.1)

    // Init reads the driver's stored network (orb_prefs::begin() has initialised NVS, and never
    // erases it)...
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err)); return false; }
    // ...and from here on it writes nothing back. Promise 1 in orb_wifi.h.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
#ifdef CONFIG_ORB_WIFI_COUNTRY
    if (strcmp(CONFIG_ORB_WIFI_COUNTRY, "01") != 0 &&
        esp_wifi_set_country_code(CONFIG_ORB_WIFI_COUNTRY, true) != ESP_OK)
        ESP_LOGW(TAG, "country code '%s' was not accepted", CONFIG_ORB_WIFI_COUNTRY);
#endif

    // The saved list. The first time this firmware runs there is none, so it starts from the one
    // network the Arduino firmware stored in the driver, and nobody re-enters anything.
    s_listPersisted = orb_prefs::available();
    if (!orb_prefs::getBlob(LIST_KEY, &s_list, sizeof(s_list)) || s_list.version != LIST_VERSION ||
        s_list.count > MAX_SAVED) {
        memset(&s_list, 0, sizeof(s_list));
        s_list.version = LIST_VERSION;
        wifi_config_t stored = {};
        if (esp_wifi_get_config(WIFI_IF_STA, &stored) == ESP_OK && stored.sta.ssid[0]) {
            // The driver's fields need no terminator (see attempt()), so bound the copy by them.
            memcpy(s_list.nets[0].ssid, stored.sta.ssid, strnlen((const char *)stored.sta.ssid, sizeof(stored.sta.ssid)));
            memcpy(s_list.nets[0].pass, stored.sta.password,
                   strnlen((const char *)stored.sta.password, sizeof(stored.sta.password)));
            s_list.count = 1;
            printf("[wifi] saved list started from the network already on this Orb: '%s'\n",
                   s_list.nets[0].ssid);
        }
        save_list();
    }
    printf("[wifi] %d saved network(s)%s:", s_list.count, s_listPersisted ? "" : " (NVS unusable: RAM only)");
    for (int i = 0; i < s_list.count; ++i) printf(" '%s'", s_list.nets[i].ssid);
    printf("\n");

#ifdef CONFIG_ORB_DEV_WIFI_SSID
    // A development build's network: one more candidate, in RAM, never saved.
    if (CONFIG_ORB_DEV_WIFI_SSID[0]) {
        copy_str(s_dev.ssid, sizeof(s_dev.ssid), CONFIG_ORB_DEV_WIFI_SSID);
        copy_str(s_dev.pass, sizeof(s_dev.pass), CONFIG_ORB_DEV_WIFI_PASS);
        s_haveDev = true;
        printf("[wifi] development build: '%s' is a candidate too (RAM only, never saved)\n", s_dev.ssid);
    }
#endif

    const esp_timer_create_args_t t = { .callback = timer_cb, .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK, .name = "wifi",
                                        .skip_unhandled_events = true };
    esp_timer_create(&t, &s_timer);
    const esp_timer_create_args_t pt = { .callback = [](void *) { post(EV_PORTAL_CLOSE); }, .arg = nullptr,
                                         .dispatch_method = ESP_TIMER_TASK, .name = "wifi_portal",
                                         .skip_unhandled_events = true };
    esp_timer_create(&pt, &s_portalTimer);
    s_offlineSinceUs = esp_timer_get_time();   // boot counts as "not yet connected"
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);
    // Core 0, with the rest of the network. Scan records are on the heap, so 4 KB is ample.
    xTaskCreatePinnedToCore(manager, "wifi_mgr", 4096, nullptr, 3, nullptr, 0);

    err = esp_wifi_start();
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err)); return false; }
    return true;
}

orb_wifi::State orb_wifi::state() { return s_state; }
bool orb_wifi::connected() { return s_state == State::Connected; }

int orb_wifi::rssi() {
    if (!connected()) return -127;
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : -127;
}

void orb_wifi::currentSsid(char *out, size_t n) {
    if (!n) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_str(out, n, connected() ? s_current : "");
    xSemaphoreGive(s_lock);
}

void orb_wifi::savedSsid(char *out, size_t n) {
    if (!n) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_str(out, n, s_current[0] ? s_current : (s_list.count ? s_list.nets[0].ssid : ""));
    xSemaphoreGive(s_lock);
}

bool orb_wifi::timeSynced() { return s_synced; }

int orb_wifi::savedCount() { return s_list.count; }

bool orb_wifi::savedAt(int i, char *ssid, size_t n) {
    if (!n) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = i >= 0 && i < s_list.count;
    copy_str(ssid, n, ok ? s_list.nets[i].ssid : "");
    xSemaphoreGive(s_lock);
    return ok;
}

bool orb_wifi::forget(const char *ssid) {
    if (!ssid) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int at = find_saved(ssid);
    if (at >= 0) {
        memmove(&s_list.nets[at], &s_list.nets[at + 1], sizeof(Net) * (s_list.count - at - 1));
        --s_list.count;
        memset(&s_list.nets[s_list.count], 0, sizeof(Net));
    }
    const int count = s_list.count;
    xSemaphoreGive(s_lock);
    if (at < 0) return false;
    save_list();
    printf("[wifi] forgot '%s'; %d saved\n", ssid, count);
    post(EV_FORGET);
    return true;
}

bool orb_wifi::tryNetwork(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) return false;
    if (s_try == TryResult::Trying) return false;
    memset(&s_tryNet, 0, sizeof(s_tryNet));
    copy_str(s_tryNet.ssid, sizeof(s_tryNet.ssid), ssid);
    copy_str(s_tryNet.pass, sizeof(s_tryNet.pass), pass ? pass : "");
    s_try = TryResult::Trying;
    post(EV_TRY);
    return true;
}

orb_wifi::TryResult orb_wifi::tryResult() { return s_try; }

void orb_wifi::startScan() { s_scanState = -1; post(EV_SCAN); }
// scanResults() answers from the last scan, a round's or a picker's, whenever there is one, so
// a picker that opens shows a list at once; startScan() is for "look again".

int orb_wifi::scanResults(char names[][33], int8_t *rssi, bool *isOpen, int maxN) {
    const int st = s_scanState;
    if (st < 0) return st;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = st < maxN ? st : maxN;
    for (int i = 0; i < n; ++i) {
        copy_str(names[i], 33, s_scan[i].ssid);
        rssi[i] = s_scan[i].rssi;
        isOpen[i] = s_scan[i].open;
    }
    xSemaphoreGive(s_lock);
    return n;
}
