// The Orb OS — themes from a computer, over WiFi, for development.
//
// A development build with CONFIG_ORB_DEV_THEME_URL set (only ever in the git-ignored
// sdkconfig.dev) asks that address once per boot, after WiFi is up, for the themes
// `tools/theme_tool.py serve` offers, and copies each into /themes/<slug>/ on the card. That is
// how a theme reaches an Orb whose card the computer cannot read.
//
// What it may do to the card, and nothing else: create /themes/<slug>/ and write the files the
// index names into it. It never deletes, never touches another folder, and accepts only plain
// names (letters, digits, '-', '_' and '.', no path). Each file goes to a temporary name and is
// renamed into place once whole; `_installed`, which is what makes the Orb list a theme, is
// written last, so a download cut short is never offered in Settings.
#include "dev_themes.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "orb_net.h"
#include "orb_platform.h"   // ORB_SD_ROOT
#include "orb_wifi.h"
#include "sdcard.h"

#ifndef CONFIG_ORB_DEV_THEME_URL
#define CONFIG_ORB_DEV_THEME_URL ""
#endif

namespace {

bool plain_name(const char *s) {
    if (!s || !s[0] || s[0] == '.' || strlen(s) > 40) return false;
    for (const char *p = s; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') return false;
    return true;
}

// The body straight to a file, chunk by chunk, on orb_net's client task.
bool to_file(orb_net::Body &body, void *ctx) {
    FILE *f = (FILE *)ctx;
    char buf[1024];
    while (size_t n = body.readChunk(buf, sizeof(buf)))
        if (fwrite(buf, 1, n, f) != n) return false;
    return true;
}

bool download(const char *slug, const char *name) {
    char url[192], tmp[128], path[128];
    snprintf(url, sizeof(url), "%s/%s/%s", CONFIG_ORB_DEV_THEME_URL, slug, name);
    snprintf(path, sizeof(path), ORB_SD_ROOT "/themes/%s/%s", slug, name);
    snprintf(tmp, sizeof(tmp), ORB_SD_ROOT "/themes/%s/~download", slug);
    FILE *f = fopen(tmp, "wb");
    if (!f) { printf("[dev_themes] cannot write %s\n", tmp); return false; }
    orb_net::Request req;
    req.url = url;
    req.app = orb_net::App::Themes;
    req.timeoutMs = 20000;
    req.maxBytes = 2 * 1024 * 1024;
    req.handler = to_file;
    req.ctx = f;
    const int status = orb_net::fetch(req);
    const bool closed = fclose(f) == 0;
    if (status != 200 || !closed) {
        printf("[dev_themes] %s/%s: %d\n", slug, name, status);
        remove(tmp);
        return false;
    }
    remove(path);   // FAT's rename does not replace
    return rename(tmp, path) == 0;
}

struct Index { JsonDocument *doc; };

void run() {
    char url[160];
    snprintf(url, sizeof(url), "%s/index.json", CONFIG_ORB_DEV_THEME_URL);
    JsonDocument doc(orb_net::allocator(orb_net::App::Themes));
    Index idx{&doc};
    orb_net::Request req;
    req.url = url;
    req.app = orb_net::App::Themes;
    req.timeoutMs = 8000;
    req.maxBytes = 32 * 1024;
    req.ctx = &idx;
    req.handler = [](orb_net::Body &body, void *ctx) {
        return !deserializeJson(*((Index *)ctx)->doc, body);
    };
    const int status = orb_net::fetch(req);
    if (status != 200) {
        printf("[dev_themes] no theme server at %s (%d)\n", CONFIG_ORB_DEV_THEME_URL, status);
        return;
    }
    mkdir(ORB_SD_ROOT "/themes", 0775);
    for (JsonPairConst theme : doc["themes"].as<JsonObjectConst>()) {
        const char *slug = theme.key().c_str();
        if (!plain_name(slug) || strlen(slug) > 31) { printf("[dev_themes] skipping '%s'\n", slug); continue; }
        char dir[96];
        snprintf(dir, sizeof(dir), ORB_SD_ROOT "/themes/%s", slug);
        mkdir(dir, 0775);
        int got = 0, failed = 0;
        for (JsonVariantConst v : theme.value().as<JsonArrayConst>()) {
            const char *name = v.as<const char *>();
            if (!plain_name(name) || !strcmp(name, "_installed")) { ++failed; continue; }
            if (download(slug, name)) ++got; else ++failed;
        }
        if (failed) {
            printf("[dev_themes] '%s': %d file(s) failed; not marked installed\n", slug, failed);
            continue;
        }
        char marker[112];
        snprintf(marker, sizeof(marker), "%s/_installed", dir);
        if (FILE *m = fopen(marker, "wb")) fclose(m);
        printf("[dev_themes] '%s': %d file(s) copied; choose it in Settings > Theme\n", slug, got);
    }
}

void task(void *) {
    while (!orb_wifi::connected()) vTaskDelay(pdMS_TO_TICKS(1000));
    vTaskDelay(pdMS_TO_TICKS(2000));   // let the first feed poll and NTP go first
    run();
    vTaskDelete(nullptr);
}

}  // namespace

void dev_themes::start() {
    if (!CONFIG_ORB_DEV_THEME_URL[0]) return;
    if (!sdcard::mounted()) { printf("[dev_themes] no card to copy themes onto\n"); return; }
    printf("[dev_themes] development build: themes from %s once WiFi is up\n", CONFIG_ORB_DEV_THEME_URL);
    xTaskCreatePinnedToCore(task, "dev_themes", 6144, nullptr, 1, nullptr, 0);
}
