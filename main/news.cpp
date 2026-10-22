// The Orb OS — Headlines from NRK and VG, on ESP-IDF. Stage 7 of REWRITE_PLAN.md.
//
// The Headlines screen (src/intel_view.cpp) asks for its stories through intel_fetch() and
// intel_brief_fetch() (src/intel_client.h). On Arduino those went to the Orb Studio gateway,
// which is gone; here they read the publishers' RSS feeds directly, over HTTPS through
// orb_net, and the screen itself is unchanged.
//
//  - Feeds: NRK and VG, picked by the theme's topic (general, world, sports, science...) and
//    by the owner's choice in Settings > News (both, NRK, VG; NVS "newsSrc"). A card that
//    carries /news.json chooses its own feeds instead (see docs/news.md).
//  - Parsed with expat as the bytes arrive; at most 20 stories are read from each feed and
//    the rest of a long feed is never downloaded. Full UTF-8 is kept.
//  - Merged newest first by <pubDate>; each story is credited to its source.
//  - The summary a press opens is the story's <description>, kept with the list (in PSRAM),
//    so it shows at once and needs no second request.
//  - Conditional requests (orb_net's cache), and never more than one round in five minutes
//    whatever the theme's pollMinutes says: a feed that has not changed costs a 304.
#include "news.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ArduinoJson.h>
#include <expat.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "intel.h"
#include "intel_client.h"
#include "orb_net.h"
#include "orb_platform.h"
#include "orb_prefs.h"

namespace {

constexpr int      PER_FEED   = 20;                     // stories read from each feed
constexpr int      MAX_FEEDS  = 6;
constexpr uint32_t MIN_ROUND_MS = 5 * 60 * 1000;        // REWRITE_PLAN.md: poll >= 5 minutes
constexpr int      HEADLINE_CHARS = 70;                 // what the gateway cut to; the screen is sized for it

struct Feed { char label[INTEL_SOURCE_BYTES]; char url[160]; };

// One story as read from a feed.
struct Story {
    char     title[INTEL_TEXT_BYTES];
    char     source[INTEL_SOURCE_BYTES];
    char     key[INTEL_KEY_BYTES];
    char     body[INTEL_BRIEF_BYTES];
    int64_t  when;                                      // seconds since 1970, 0 when unknown
};

// What each feed last said, kept so a 304 or a still-fresh answer reuses it. PSRAM: twenty
// stories with their summaries are about 13 KB a feed.
struct FeedCache { char url[160]; int n; Story stories[PER_FEED]; };
FeedCache *s_cache = nullptr;      // MAX_FEEDS of them
Story     *s_merged = nullptr;     // INTEL_MAX_ITEMS, the list the screen holds
int        s_mergedN = 0;
int64_t    s_lastRoundUs = 0;

// --- text --------------------------------------------------------------------------------

uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

// Whitespace collapsed, HTML tags (some descriptions carry them) left out, in place.
void tidy(char *s) {
    char *o = s;
    bool space = true, tag = false;
    for (const char *p = s; *p; ++p) {
        if (tag) { if (*p == '>') tag = false; continue; }
        if (*p == '<') { tag = true; continue; }
        if (isspace((unsigned char)*p)) { if (!space) *o++ = ' '; space = true; continue; }
        *o++ = *p;
        space = false;
    }
    if (o > s && o[-1] == ' ') --o;
    *o = 0;
}

// Copy as much of `src` as fits, ending on a whole UTF-8 character.
void copy_utf8(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) --n;   // not mid-character
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

// Cut to `chars` characters on a word boundary, with an ellipsis, never inside a UTF-8
// sequence. The gateway did this for the screen; the buffer is sized for it.
void cut(char *s, size_t cap, int chars) {
    int n = 0;
    size_t i = 0, lastSpace = 0;
    for (; s[i]; ++i) {
        if (((unsigned char)s[i] & 0xC0) == 0x80) continue;   // a continuation byte
        if (n == chars) break;
        if (s[i] == ' ') lastSpace = i;
        ++n;
    }
    if (!s[i] && i < cap) return;                              // it fits
    size_t at = lastSpace > i / 2 ? lastSpace : i;
    while (at > 0 && ((unsigned char)s[at] & 0xC0) == 0x80) --at;
    if (at + 4 > cap) at = cap - 4;
    while (at > 0 && ((unsigned char)s[at] & 0xC0) == 0x80) --at;
    memcpy(s + at, "\xE2\x80\xA6", 4);                         // …
}

// "Thu, 22 Oct 2026 19:37:23 GMT" or "+0200" to seconds since 1970; 0 when it does not read.
int64_t parse_date(const char *s) {
    int d, y, hh, mm, ss = 0;
    char mon[4], zone[8] = "";
    if (!s || sscanf(s, "%*3s, %d %3s %d %d:%d:%d %7s", &d, mon, &y, &hh, &mm, &ss, zone) < 6) return 0;
    static const char *M = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *at = strstr(M, mon);
    if (!at || (at - M) % 3) return 0;
    int m = (int)(at - M) / 3 + 1;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t t = ((int64_t)era * 146097 + doe - 719468) * 86400 + hh * 3600 + mm * 60 + ss;
    if ((zone[0] == '+' || zone[0] == '-') && strlen(zone) == 5) {
        const int off = (zone[1] - '0') * 600 + (zone[2] - '0') * 60 + (zone[3] - '0') * 10 + (zone[4] - '0');
        t -= (zone[0] == '-' ? -1 : 1) * (int64_t)off * 60;
    }
    return t;
}

// --- RSS, through expat --------------------------------------------------------------------

enum Field { F_NONE, F_TITLE, F_DESC, F_LINK, F_DATE };

struct Parse {
    FeedCache  *out;
    const char *label;
    bool        inItem = false;
    Field       field = F_NONE;
    char        text[INTEL_BRIEF_BYTES];   // the field being read
    size_t      len = 0;
    Story       cur;
    char        link[160];
    char        date[40];
};

void XMLCALL on_start(void *ud, const XML_Char *name, const XML_Char **) {
    Parse *p = (Parse *)ud;
    if (!strcmp(name, "item")) {
        p->inItem = true;
        memset(&p->cur, 0, sizeof(p->cur));
        p->link[0] = p->date[0] = 0;
        return;
    }
    if (!p->inItem) return;
    p->field = !strcmp(name, "title") ? F_TITLE : !strcmp(name, "description") ? F_DESC
             : !strcmp(name, "link") ? F_LINK : !strcmp(name, "pubDate") ? F_DATE : F_NONE;
    p->len = 0;
    p->text[0] = 0;
}

void XMLCALL on_text(void *ud, const XML_Char *s, int n) {
    Parse *p = (Parse *)ud;
    if (p->field == F_NONE) return;
    const size_t room = sizeof(p->text) - 1 - p->len;
    const size_t take = (size_t)n < room ? (size_t)n : room;
    memcpy(p->text + p->len, s, take);
    p->len += take;
    p->text[p->len] = 0;
}

void XMLCALL on_end(void *ud, const XML_Char *name) {
    Parse *p = (Parse *)ud;
    if (p->inItem && p->field != F_NONE) {
        tidy(p->text);
        switch (p->field) {
            case F_TITLE: copy_utf8(p->cur.title, sizeof(p->cur.title), p->text); break;
            case F_DESC:  copy_utf8(p->cur.body, sizeof(p->cur.body), p->text); break;
            case F_LINK:  copy_utf8(p->link, sizeof(p->link), p->text); break;
            case F_DATE:  copy_utf8(p->date, sizeof(p->date), p->text); break;
            default: break;
        }
        p->field = F_NONE;
        return;
    }
    if (strcmp(name, "item") || !p->inItem) return;
    p->inItem = false;
    if (!p->cur.title[0] || p->out->n >= PER_FEED) return;
    Story &st = p->out->stories[p->out->n++];
    st = p->cur;
    cut(st.title, sizeof(st.title), HEADLINE_CHARS);
    snprintf(st.source, sizeof(st.source), "%s", p->label);
    snprintf(st.key, sizeof(st.key), "%08lx", (unsigned long)fnv(p->link[0] ? p->link : st.title));
    st.when = parse_date(p->date);
}

bool read_rss(orb_net::Body &body, void *ctx) {
    Parse *p = (Parse *)ctx;
    XML_Parser x = XML_ParserCreate(nullptr);
    if (!x) return false;
    XML_SetUserData(x, p);
    XML_SetElementHandler(x, on_start, on_end);
    XML_SetCharacterDataHandler(x, on_text);
    char buf[1024];
    bool ok = true;
    while (p->out->n < PER_FEED) {                     // and not one byte more than that needs
        const size_t n = body.readChunk(buf, sizeof(buf));
        if (XML_Parse(x, buf, (int)n, n == 0) == XML_STATUS_ERROR) {
            printf("[news] %s: XML error at line %lu: %s\n", p->label,
                   (unsigned long)XML_GetCurrentLineNumber(x), XML_ErrorString(XML_GetErrorCode(x)));
            ok = p->out->n > 0;                        // what was read before the fault still stands
            break;
        }
        if (n == 0) break;
    }
    XML_ParserFree(x);
    return ok;
}

// --- feeds -----------------------------------------------------------------------------------

// 0 both, 1 NRK, 2 VG: Settings > News.
int sources() {
    const int v = (int)orb_prefs::getInt("newsSrc", 0);
    return v >= 0 && v <= 2 ? v : 0;
}

// /news.json on the card: {"feeds": [{"label": "NRK", "url": "https://..."}, ...]}.
int feeds_from_card(Feed *out) {
    struct stat st;
    if (stat(ORB_SD_ROOT "/news.json", &st) != 0 || st.st_size > 4096) return 0;
    FILE *f = fopen(ORB_SD_ROOT "/news.json", "rb");
    if (!f) return 0;
    static char text[4097];
    const size_t len = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, text, len);
    if (err) { printf("[news] /news.json is not valid JSON (%s); using the built-in feeds\n", err.c_str()); return 0; }
    int n = 0;
    for (JsonObjectConst o : doc["feeds"].as<JsonArrayConst>()) {
        const char *url = o["url"] | "", *label = o["label"] | "";
        if (n >= MAX_FEEDS || strncmp(url, "https://", 8) || !label[0]) continue;
        snprintf(out[n].label, sizeof(out[n].label), "%s", label);
        snprintf(out[n].url, sizeof(out[n].url), "%s", url);
        ++n;
    }
    return n;
}

int feeds_for(const char *topic, Feed *out) {
    if (const int n = feeds_from_card(out)) return n;
    const bool sports = topic && !strcmp(topic, "sports");
    const bool world = topic && !strcmp(topic, "world");
    const bool science = topic && (!strcmp(topic, "science") || !strcmp(topic, "tech") || !strcmp(topic, "space"));
    const int src = sources();
    int n = 0;
    if (src != 2) {
        snprintf(out[n].label, sizeof(out[n].label), "NRK");
        snprintf(out[n].url, sizeof(out[n].url), "https://www.nrk.no/%stoppsaker.rss",
                 sports ? "sport/" : world ? "urix/" : science ? "viten/" : "");
        ++n;
    }
    if (src != 1 && !science) {   // VG has no science section of its own
        snprintf(out[n].label, sizeof(out[n].label), "VG");
        snprintf(out[n].url, sizeof(out[n].url), "https://www.vg.no/rss/feed/?categories=%d",
                 sports ? 1072 : world ? 1070 : 1068);
        ++n;
    }
    return n;
}

FeedCache *cache_for(const char *url) {
    FeedCache *empty = nullptr;
    for (int i = 0; i < MAX_FEEDS; ++i) {
        if (!strcmp(s_cache[i].url, url)) return &s_cache[i];
        if (!empty && !s_cache[i].url[0]) empty = &s_cache[i];
    }
    if (!empty) { empty = &s_cache[0]; }               // a seventh feed: reuse the first slot
    memset(empty, 0, sizeof(*empty));
    snprintf(empty->url, sizeof(empty->url), "%s", url);
    return empty;
}

bool ensure_memory() {
    if (s_cache) return true;
    s_cache = (FeedCache *)heap_caps_calloc(MAX_FEEDS, sizeof(FeedCache), MALLOC_CAP_SPIRAM);
    s_merged = (Story *)heap_caps_calloc(INTEL_MAX_ITEMS, sizeof(Story), MALLOC_CAP_SPIRAM);
    return s_cache && s_merged;
}

}  // namespace

// --- the screen's two calls (src/intel_client.h) ---------------------------------------------

bool intel_fetch(const char *topic, const char *, int want, IntelSnapshot &out) {
    if (!ensure_memory()) return false;
    const int64_t now = esp_timer_get_time();
    const bool tooSoon = s_lastRoundUs && now - s_lastRoundUs < (int64_t)MIN_ROUND_MS * 1000;

    Feed feeds[MAX_FEEDS];
    const int nFeeds = feeds_for(topic, feeds);
    int answered = 0;
    for (int f = 0; f < nFeeds; ++f) {
        FeedCache *c = cache_for(feeds[f].url);
        if (tooSoon && c->n) { ++answered; continue; }
        // About 11 KB each: PSRAM, never this task's stack.
        FeedCache *fresh = (FeedCache *)heap_caps_calloc(1, sizeof(FeedCache), MALLOC_CAP_SPIRAM);
        Parse *p = (Parse *)heap_caps_calloc(1, sizeof(Parse), MALLOC_CAP_SPIRAM);
        if (!fresh || !p) { heap_caps_free(fresh); heap_caps_free(p); continue; }
        snprintf(fresh->url, sizeof(fresh->url), "%s", c->url);
        p->out = fresh;
        p->label = feeds[f].label;
        orb_net::Request req;
        req.url = feeds[f].url;
        req.app = orb_net::App::News;
        req.accept = "application/rss+xml, application/xml";
        req.timeoutMs = 15000;
        req.maxBytes = 256 * 1024;
        req.cached = true;
        req.handler = read_rss;
        req.ctx = p;
        const int status = orb_net::fetch(req);
        heap_caps_free(p);
        if (status == 200 && fresh->n) { *c = *fresh; ++answered; }
        else if ((status == 304 || status == orb_net::FRESH) && c->n) ++answered;
        heap_caps_free(fresh);
        printf("[news] %s: %d (%d stories)\n", feeds[f].label, status, c->n);
    }
    if (!tooSoon) s_lastRoundUs = now;
    if (!answered) return false;

    // Newest first across every feed that answered; a story with no date sorts last.
    s_mergedN = 0;
    for (int f = 0; f < nFeeds; ++f) {
        const FeedCache *c = cache_for(feeds[f].url);
        for (int i = 0; i < c->n; ++i) {
            const Story &st = c->stories[i];
            int at = s_mergedN;
            while (at > 0 && s_merged[at - 1].when < st.when) --at;
            if (at >= INTEL_MAX_ITEMS) continue;
            const int last = s_mergedN < INTEL_MAX_ITEMS ? s_mergedN : INTEL_MAX_ITEMS - 1;
            memmove(&s_merged[at + 1], &s_merged[at], (size_t)(last - at) * sizeof(Story));
            s_merged[at] = st;
            if (s_mergedN < INTEL_MAX_ITEMS) ++s_mergedN;
        }
    }
    const int n = want < s_mergedN ? (want > 0 ? want : s_mergedN) : s_mergedN;
    static IntelSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    for (int i = 0; i < n && i < INTEL_MAX_ITEMS; ++i) {
        IntelItem &it = snap.items[snap.count++];
        snprintf(it.text, sizeof(it.text), "%s", s_merged[i].title);
        snprintf(it.source, sizeof(it.source), "%s", s_merged[i].source);
        snprintf(it.key, sizeof(it.key), "%s", s_merged[i].key);
    }
    snap.valid = snap.count > 0;
    snap.fetchedMs = millis();
    out = snap;
    return snap.valid;
}

IntelBriefState intel_brief_fetch(const char *, const char *, const char *key,
                                  char *headlineOut, size_t headlineCap,
                                  char *sourceOut, size_t sourceCap,
                                  char *bodyOut, size_t bodyCap) {
    for (int i = 0; s_merged && i < s_mergedN; ++i) {
        const Story &st = s_merged[i];
        if (strcmp(st.key, key)) continue;
        snprintf(headlineOut, headlineCap, "%s", st.title);
        snprintf(sourceOut, sourceCap, "%s", st.source);
        snprintf(bodyOut, bodyCap, "%s", st.body);
        return st.body[0] ? INTEL_BRIEF_READY : INTEL_BRIEF_EMPTY;
    }
    return INTEL_BRIEF_GONE;
}

// --- Settings > News -------------------------------------------------------------------------

int host_news_sources() { return sources(); }
void host_news_sources_set(int v) {
    orb_prefs::putInt("newsSrc", v >= 0 && v <= 2 ? v : 0);
    s_lastRoundUs = 0;   // a new choice is fetched now, not in five minutes
}
