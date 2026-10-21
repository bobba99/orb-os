// The Orb OS — saved settings on ESP-IDF. See orb_prefs.h for the two promises this keeps.
#include "orb_prefs.h"

#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "prefs";
static const char *NAMESPACE = "capsuleradar";   // see orb_prefs.h: never anything else

static bool s_ok = false;

bool orb_prefs::begin() {
    // nvs_flash_init(), and deliberately nothing after it on failure. No erase-and-retry:
    // see orb_prefs.h.
    const esp_err_t err = nvs_flash_init();
    s_ok = (err == ESP_OK);
    if (!s_ok)
        ESP_LOGE(TAG, "NVS would not initialise (%s). Running on defaults; nothing will be "
                      "saved, and nothing has been erased.", esp_err_to_name(err));
    return s_ok;
}

bool orb_prefs::available() { return s_ok; }

// One handle per call, opened and closed around it, as Arduino code used Preferences: these
// are boot-time and settings-change operations, never in a render path.
namespace {
struct Handle {
    nvs_handle_t h = 0;
    bool open = false;
    explicit Handle(bool write) {
        if (s_ok) open = nvs_open(NAMESPACE, write ? NVS_READWRITE : NVS_READONLY, &h) == ESP_OK;
    }
    ~Handle() { if (open) nvs_close(h); }
};

bool commit(Handle &w, esp_err_t err, const char *key) {
    if (err == ESP_OK) err = nvs_commit(w.h);
    if (err != ESP_OK) ESP_LOGE(TAG, "write of '%s' failed: %s", key, esp_err_to_name(err));
    return err == ESP_OK;
}
}  // namespace

bool orb_prefs::getString(const char *key, char *out, size_t outLen, const char *def) {
    if (!outLen) return false;
    Handle r(false);
    size_t n = outLen;
    if (r.open && nvs_get_str(r.h, key, out, &n) == ESP_OK) return true;
    // Too long for `out`, absent, or another type: the default, never a truncated value
    // that looks like a real one.
    strncpy(out, def ? def : "", outLen - 1);
    out[outLen - 1] = '\0';
    return false;
}

int32_t orb_prefs::getInt(const char *key, int32_t def) {
    Handle r(false);
    int32_t v = def;
    if (!r.open || nvs_get_i32(r.h, key, &v) != ESP_OK) v = def;
    return v;
}

uint32_t orb_prefs::getUInt(const char *key, uint32_t def) {
    Handle r(false);
    uint32_t v = def;
    if (!r.open || nvs_get_u32(r.h, key, &v) != ESP_OK) v = def;
    return v;
}

int64_t orb_prefs::getInt64(const char *key, int64_t def) {
    Handle r(false);
    int64_t v = def;
    if (!r.open || nvs_get_i64(r.h, key, &v) != ESP_OK) v = def;
    return v;
}

// Arduino stores a float or a double as a blob of exactly its size. Anything else under that
// key (another type, a different length) is not that value, so it reads as the default.
template <typename T> static T get_blob(const char *key, T def) {
    Handle r(false);
    T v = def;
    size_t n = sizeof(T);
    if (!r.open || nvs_get_blob(r.h, key, &v, &n) != ESP_OK || n != sizeof(T)) return def;
    return v;
}
float  orb_prefs::getFloat(const char *key, float def)   { return get_blob(key, def); }
double orb_prefs::getDouble(const char *key, double def) { return get_blob(key, def); }

bool orb_prefs::getBool(const char *key, bool def) {
    Handle r(false);
    uint8_t v = def ? 1 : 0;
    if (!r.open || nvs_get_u8(r.h, key, &v) != ESP_OK) return def;
    return v != 0;
}

bool orb_prefs::isKey(const char *key) {
    Handle r(false);
    nvs_type_t t;
    return r.open && nvs_find_key(r.h, key, &t) == ESP_OK;
}

bool orb_prefs::putString(const char *key, const char *value) {
    Handle w(true);
    return w.open && commit(w, nvs_set_str(w.h, key, value ? value : ""), key);
}

bool orb_prefs::putInt(const char *key, int32_t value) {
    Handle w(true);
    return w.open && commit(w, nvs_set_i32(w.h, key, value), key);
}

bool orb_prefs::putUInt(const char *key, uint32_t value) {
    Handle w(true);
    return w.open && commit(w, nvs_set_u32(w.h, key, value), key);
}

bool orb_prefs::putInt64(const char *key, int64_t value) {
    Handle w(true);
    return w.open && commit(w, nvs_set_i64(w.h, key, value), key);
}

bool orb_prefs::putFloat(const char *key, float value) {
    Handle w(true);
    return w.open && commit(w, nvs_set_blob(w.h, key, &value, sizeof(value)), key);
}

bool orb_prefs::putDouble(const char *key, double value) {
    Handle w(true);
    return w.open && commit(w, nvs_set_blob(w.h, key, &value, sizeof(value)), key);
}

bool orb_prefs::putBool(const char *key, bool value) {
    Handle w(true);
    return w.open && commit(w, nvs_set_u8(w.h, key, value ? 1 : 0), key);
}

bool orb_prefs::remove(const char *key) {
    Handle w(true);
    if (!w.open) return false;
    const esp_err_t err = nvs_erase_key(w.h, key);   // one key, never the partition
    return err == ESP_ERR_NVS_NOT_FOUND || commit(w, err, key);
}
