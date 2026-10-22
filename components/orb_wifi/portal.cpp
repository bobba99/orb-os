// The Orb OS — "The Orb Setup": captive DNS and the setup page. See portal.h.
//
// A phone that joins the access point asks the network whether it is online. Every name it
// looks up is answered with the Orb's own address, and every page it asks for is redirected to
// the setup page, which is what makes the phone raise its "sign in to network" sheet by itself.
//
// The page talks to orb_wifi only through its public calls: startScan/scanResults for the list,
// tryNetwork for the join, tryResult for the answer. So a network typed here gets exactly the
// protection the Settings screen will: saved only once it has produced an address, and the
// Orb's other networks untouched if it does not.
//
// The access point is open, as the Arduino firmware's WiFiManager portal was: anyone in range
// while it is up could point the Orb at a network of their own. REWRITE_PLAN.md stage 10 puts
// the device's pages behind a password set during setup; until then the portal is up only
// while the Orb has no network it can reach.
#include "portal.h"
#include "orb_wifi.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "portal";

namespace {

constexpr const char *AP_URL = "http://192.168.4.1/";   // ESP-IDF's default soft-AP address

httpd_handle_t s_http = nullptr;
TaskHandle_t   s_dnsTask = nullptr;
volatile bool  s_dnsRun = false;
int            s_dnsSock = -1;

// --- DNS: every question gets 192.168.4.1 ----------------------------------------------------

void dns_task(void *) {
    uint8_t buf[512];
    while (s_dnsRun) {
        sockaddr_in from = {};
        socklen_t fromLen = sizeof(from);
        const int n = recvfrom(s_dnsSock, buf, sizeof(buf), 0, (sockaddr *)&from, &fromLen);
        if (n < 12) continue;                         // timeout, or shorter than a header
        if (buf[2] & 0x80) continue;                  // a response, not a question
        // Find the end of the first question: the name, then QTYPE and QCLASS.
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5;
        if (p > n || n + 16 > (int)sizeof(buf)) continue;
        const uint16_t qtype = (uint16_t)((buf[p - 4] << 8) | buf[p - 3]);
        buf[2] = 0x84;                                // response, authoritative
        buf[3] = 0x00;                                // no error
        buf[6] = 0; buf[7] = 0;                       // answers: set below
        buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0;
        int len = p;
        if (qtype == 1) {                             // A: our address. Anything else: no answer.
            buf[7] = 1;
            const uint8_t ans[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
            memcpy(buf + len, ans, sizeof(ans));
            len += sizeof(ans);
        }
        sendto(s_dnsSock, buf, len, 0, (sockaddr *)&from, fromLen);
    }
    vTaskDelete(nullptr);
}

bool dns_start() {
    s_dnsSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dnsSock < 0) return false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_dnsSock, (sockaddr *)&a, sizeof(a)) < 0) { close(s_dnsSock); s_dnsSock = -1; return false; }
    timeval tv = { 1, 0 };                            // wake once a second to notice a stop
    setsockopt(s_dnsSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    s_dnsRun = true;
    xTaskCreatePinnedToCore(dns_task, "dns", 3072, nullptr, 2, &s_dnsTask, 0);
    return true;
}

void dns_stop() {
    s_dnsRun = false;                                 // the task sees it within a second
    vTaskDelay(pdMS_TO_TICKS(1200));
    if (s_dnsSock >= 0) { close(s_dnsSock); s_dnsSock = -1; }
    s_dnsTask = nullptr;
}

// --- helpers -------------------------------------------------------------------------------

// JSON string body for a network name: quotes, backslashes and control bytes escaped. Names
// are 32 bytes of anything the router's owner typed.
void json_str(char *out, size_t outLen, const char *s) {
    size_t o = 0;
    for (; *s && o + 7 < outLen; ++s) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += snprintf(out + o, outLen - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

// application/x-www-form-urlencoded: one field's value, decoded. False if absent.
bool form_field(const char *body, const char *name, char *out, size_t outLen) {
    const size_t nl = strlen(name);
    for (const char *p = body; p && *p; ) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) > nl && !strncmp(p, name, nl) && p[nl] == '=') {
            size_t o = 0;
            for (const char *q = p + nl + 1; q < end && o + 1 < outLen; ++q) {
                if (*q == '+') out[o++] = ' ';
                else if (*q == '%' && q + 2 < end && isxdigit((unsigned char)q[1]) && isxdigit((unsigned char)q[2])) {
                    char hex[3] = { q[1], q[2], 0 };
                    out[o++] = (char)strtol(hex, nullptr, 16);
                    q += 2;
                } else out[o++] = *q;
            }
            out[o] = 0;
            return true;
        }
        p = amp ? amp + 1 : nullptr;
    }
    out[0] = 0;
    return false;
}

// --- the page -------------------------------------------------------------------------------

// Self-contained: the phone has no internet while it is on this network, so nothing here may
// load from anywhere else.
const char PAGE[] = R"HTML(<!doctype html><html lang=en><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>The Orb Setup</title><style>
*{box-sizing:border-box}body{font:16px system-ui,-apple-system,sans-serif;margin:0;padding:24px 18px 48px;background:#f5f5f4;color:#23201d;max-width:460px;margin:auto}
h1{font-size:22px;margin:0 0 4px}p{color:#6f6a64;margin:4px 0 18px;line-height:1.45}
.card{background:#fff;border:1px solid #e2e0dd;border-radius:14px;padding:16px;margin-bottom:14px}
label{display:block;font-size:13px;color:#6f6a64;margin:12px 0 6px}
select,input{width:100%;padding:11px;border:1px solid #d4d1cd;border-radius:9px;font:inherit;background:#fff;color:inherit}
button{width:100%;margin-top:16px;padding:12px;border:0;border-radius:9px;background:#a65e3f;color:#fff;font:600 16px system-ui}
button.sec{background:none;color:#a65e3f;border:1px solid #d4d1cd;margin-top:10px}
button:disabled{opacity:.5}#msg{margin-top:14px;line-height:1.45}.ok{color:#2f7a3c}.bad{color:#b3412a}
@media(prefers-color-scheme:dark){body{background:#151413;color:#ecebe9}.card{background:#1f1d1c;border-color:#34312e}select,input{background:#1f1d1c;border-color:#45413d}p,label{color:#a39e98}}
</style>
<h1>The Orb Setup</h1>
<p>Choose the WiFi network your Orb should use. It only keeps a network once it has actually joined it.</p>
<div class=card>
<label for=net>Network</label><select id=net><option value="" disabled selected>Looking for networks…</option></select>
<div id=hid style=display:none><label for=ssid>Network name</label><input id=ssid autocomplete=off autocapitalize=none></div>
<label for=pw>Password</label><input id=pw type=password autocomplete=off placeholder="Leave empty for an open network">
<button id=go disabled>Join</button><button class=sec id=re>Look again</button>
<div id=msg></div></div>
<p>Only 2.4 GHz networks can be seen: the Orb's radio does not do 5 GHz.</p>
<script>
const $=s=>document.querySelector(s),net=$('#net'),msg=$('#msg'),go=$('#go');
function say(t,c){msg.textContent=t;msg.className=c||''}
// The first look uses the list the Orb already has from its own last scan, so it arrives at
// once; "Look again" asks for a fresh one, which takes a few seconds.
async function scan(again){
 net.innerHTML='<option value="" disabled selected>Looking for networks…</option>';go.disabled=true;
 for(let i=0;i<40;i++){
  const r=await fetch('/scan'+(again&&!i?'?again=1':''),{cache:'no-store'}).then(r=>r.json()).catch(()=>null);
  if(r&&r.done){
   net.innerHTML='';
   for(const n of r.nets){const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+(n.open?'  (open)':'')+'  '+n.rssi+' dBm';net.appendChild(o)}
   const h=document.createElement('option');h.value='';h.textContent='Another network (hidden)…';net.appendChild(h);
   net.onchange();go.disabled=false;return}
  await new Promise(r=>setTimeout(r,700))}
 say('Could not look for networks. Try again.','bad')}
net.onchange=()=>{$('#hid').style.display=net.value?'none':'block'};
$('#re').onclick=()=>scan(true);
go.onclick=async()=>{
 const ssid=net.value||$('#ssid').value.trim();if(!ssid){say('Choose a network.','bad');return}
 go.disabled=true;say('Joining '+ssid+'…');
 const b=new URLSearchParams({ssid,pass:$('#pw').value});
 const ok=await fetch('/join',{method:'POST',body:b}).then(r=>r.ok).catch(()=>false);
 if(!ok){say('The Orb is busy. Try again in a moment.','bad');go.disabled=false;return}
 for(let i=0;i<40;i++){
  await new Promise(r=>setTimeout(r,1000));
  const s=await fetch('/status',{cache:'no-store'}).then(r=>r.json()).catch(()=>null);
  if(!s)continue;
  if(s.try==='joined'){say('Joined '+ssid+'. Your Orb is online and has kept this network. This setup network closes in a minute.','ok');return}
  if(s.try==='failed'){say('Could not join '+ssid+'. Check the password and try again. The Orb has not changed anything.','bad');go.disabled=false;return}}
 say('No answer yet. If the Orb shows the time, it has joined.','');go.disabled=false};
scan(false);
</script></html>)HTML";

esp_err_t page(httpd_req_t *r) {
    printf("[portal] page served\n");
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, PAGE, HTTPD_RESP_USE_STRLEN);
}

esp_err_t scan(httpd_req_t *r) {
    char q[16] = "";
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && strstr(q, "again")) {
        printf("[portal] fresh scan asked for\n");
        orb_wifi::startScan();
    }
    static char names[24][33];
    int8_t rssi[24];
    bool open[24];
    const int n = orb_wifi::scanResults(names, rssi, open, 24);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    if (n == -1) return httpd_resp_sendstr(r, "{\"done\":false}");
    printf("[portal] list sent: %d network(s)\n", n > 0 ? n : 0);
    httpd_resp_sendstr_chunk(r, "{\"done\":true,\"nets\":[");
    char esc[100], row[sizeof(esc) + 64];   // the escaped name plus the JSON around it
    for (int i = 0; i < (n > 0 ? n : 0); ++i) {
        json_str(esc, sizeof(esc), names[i]);
        snprintf(row, sizeof(row), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}", i ? "," : "", esc,
                 rssi[i], open[i] ? "true" : "false");
        httpd_resp_sendstr_chunk(r, row);
    }
    httpd_resp_sendstr_chunk(r, "]}");
    return httpd_resp_sendstr_chunk(r, nullptr);
}

esp_err_t join(httpd_req_t *r) {
    char body[256];
    const int len = r->content_len < (int)sizeof(body) - 1 ? r->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        const int k = httpd_req_recv(r, body + got, len - got);
        if (k <= 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "no body");
        got += k;
    }
    body[got] = 0;
    char ssid[33], pass[65];
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));
    if (!ssid[0] || !orb_wifi::tryNetwork(ssid, pass)) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "busy");
    }
    // The name, never the password, in the log.
    printf("[portal] asked to join '%s'\n", ssid);
    return httpd_resp_sendstr(r, "ok");
}

esp_err_t status(httpd_req_t *r) {
    const char *t = "none";
    switch (orb_wifi::tryResult()) {
    case orb_wifi::TryResult::Trying: t = "trying"; break;
    case orb_wifi::TryResult::Joined: t = "joined"; break;
    case orb_wifi::TryResult::Failed: t = "failed"; break;
    default: break;
    }
    char out[96];
    snprintf(out, sizeof(out), "{\"state\":\"%s\",\"try\":\"%s\"}", orb_wifi::stateName(orb_wifi::state()), t);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, out);
}

// Everything else, including every phone's "am I online" probe: to the setup page. A redirect
// rather than the page itself, so the phone sees an answer that is not the one it expected.
esp_err_t to_page(httpd_req_t *r, httpd_err_code_t) {
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", AP_URL);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, nullptr, 0);
}

}  // namespace

bool portal::start() {
    if (s_http) return true;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 0;
    cfg.lru_purge_enable = true;    // phones open many sockets while probing; drop the oldest
    cfg.max_open_sockets = 6;
    cfg.stack_size = 6144;
    if (httpd_start(&s_http, &cfg) != ESP_OK) { s_http = nullptr; ESP_LOGE(TAG, "web server"); return false; }
    const httpd_uri_t uris[] = {
        { "/",       HTTP_GET,  page,   nullptr },
        { "/scan",   HTTP_GET,  scan,   nullptr },
        { "/join",   HTTP_POST, join,   nullptr },
        { "/status", HTTP_GET,  status, nullptr },
    };
    for (const auto &u : uris) httpd_register_uri_handler(s_http, &u);
    httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, to_page);
    if (!dns_start()) ESP_LOGW(TAG, "DNS responder did not start; phones will not pop the page up");
    printf("[portal] setup page at %s\n", AP_URL);
    return true;
}

void portal::stop() {
    if (!s_http) return;
    dns_stop();
    httpd_stop(s_http);
    s_http = nullptr;
    printf("[portal] closed\n");
}

bool portal::running() { return s_http != nullptr; }
