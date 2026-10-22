// The Orb OS — is the active theme whole? Stage 5 of REWRITE_PLAN.md.
//
// theme_style.cpp loads a theme forgivingly: a file that is missing, does not parse, or is
// too big is read as "use the defaults", and nothing says so. That is right for drawing and
// wrong for the person who made the theme, who sees their design half-applied with no clue
// why. This looks at the same files once at boot and names the first thing wrong, for a
// notice on screen (settingsview::openThemeProblem) and the log. It changes nothing.
//
// The rules are docs/themes.md's; tools/theme_tool.py checks the same ones on a computer.
#include "theme_check.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <ArduinoJson.h>

#include "orb_platform.h"   // ORB_SD_ROOT
#include "theme_art.h"
#include "theme_sd.h"
#include "theme_select.h"

namespace {

constexpr int    SCHEMA = 1;              // the theme format this firmware reads
constexpr size_t STYLE_MAX = 8192;        // theme_style.cpp's MAX_STYLE_JSON_BYTES
const char *const STYLE_FILES[] = {
    "clock_style.json", "radar_style.json", "settings_style.json", "menu_style.json",
    "splash_style.json", "intel_style.json", "ticker_style.json", "weather_style.json",
};

char s_problem[200];

bool on_card(const char *slug, const char *name, size_t *size = nullptr) {
    char path[128];
    snprintf(path, sizeof(path), ORB_SD_ROOT "/themes/%s/%s", slug, name);
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (size) *size = (size_t)st.st_size;
    return true;
}

const char *problem(const char *fmt, const char *a, const char *b = "") {
    snprintf(s_problem, sizeof(s_problem), fmt, a, b);
    printf("[theme] problem: %s\n", s_problem);
    return s_problem;
}

// Read one JSON file of the theme. False with a problem set when it is there but unusable.
bool read_json(const char *slug, const char *name, JsonDocument &doc, bool &present) {
    size_t size = 0;
    present = on_card(slug, name, &size);
    if (!present) return true;
    if (size > STYLE_MAX) {
        char kb[16];
        snprintf(kb, sizeof(kb), "%u", (unsigned)(size / 1024));
        problem("%s is %s KB; the Orb reads at most 8 KB, so it is ignored.", name, kb);
        return false;
    }
    char path[96];
    snprintf(path, sizeof(path), "/themes/%s/%s", slug, name);
    size_t len = 0;
    uint8_t *buf = theme_sd::read_whole(path, len, STYLE_MAX);
    if (!buf) { problem("%s could not be read from the card.", name); return false; }
    const DeserializationError err = deserializeJson(doc, buf, len);
    theme_sd::free(buf);
    if (err) { problem("%s is not valid JSON (%s), so it is ignored.", name, err.c_str()); return false; }
    return true;
}

}  // namespace

const char *theme_check::run() {
    s_problem[0] = 0;
    const char *slug = theme_select::activeSlug();
    if (!slug || !slug[0]) return nullptr;   // the compiled look: nothing to check

    JsonDocument theme;
    bool present = false;
    if (!read_json(slug, "theme.json", theme, present)) return s_problem;
    if (!present) return problem("theme.json is missing from /themes/%s.", slug);

    // The format's version. Absent is an Orb Studio export, which is what schema 1 describes.
    JsonVariantConst schema = theme["schema"];
    if (schema.isNull()) {
        printf("[theme] '%s' has no \"schema\": an Orb Studio export, read as schema %d\n", slug, SCHEMA);
    } else if (!schema.is<int>() || schema.as<int>() != SCHEMA) {
        char got[40] = "something other than a number", want[8];
        if (schema.is<int>()) snprintf(got, sizeof(got), "%d", schema.as<int>());
        snprintf(want, sizeof(want), "%d", SCHEMA);
        return problem("theme.json says \"schema\": %s; this firmware reads schema %s.", got, want);
    }

    for (const char *name : STYLE_FILES) {
        JsonDocument doc;
        if (!read_json(slug, name, doc, present)) return s_problem;
    }

    // Every picture and font the theme declares is on the card, or already baked into flash.
    int missing = 0;
    char first[40] = "";
    for (JsonVariantConst v : theme["assets"].as<JsonArrayConst>()) {
        const char *name = v.as<const char *>();
        if (!name || !name[0]) continue;
        if (on_card(slug, name) || theme_art::has(slug, name)) continue;
        if (!missing++) snprintf(first, sizeof(first), "%s", name);
    }
    if (missing) {
        char more[32] = "";
        if (missing > 1) snprintf(more, sizeof(more), " and %d more", missing - 1);
        return problem("%s%s are declared in theme.json but not on the card.", first, more);
    }
    printf("[theme] '%s' checks out\n", slug);
    return nullptr;
}
