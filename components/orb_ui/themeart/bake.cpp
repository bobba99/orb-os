// The Orb OS — baking the active theme into the themeart partition, on ESP-IDF.
//
// src/theme_art_bake.cpp's job (read each PNG off the card once, unpack it, store the pixels
// in flash) with its rules kept: only what the theme declares, fonts first, animation frames
// before the plates, and a second pass with the whole partition when something did not fit.
// What is new is the choice of format, per picture:
//
//  - A picture of 256 colours or fewer (counting each colour-and-transparency pair once) is
//    stored indexed: palette + 1, 2, 4 or 8 bits a pixel. A typical plate has under a
//    hundred colours and halves; a one-colour glass with one level of transparency drops
//    from 3 bytes a pixel to 1 bit.
//  - Anything else is stored as before, RGB565 or RGB565+A8.
//
// Only pictures whose every reader understands the indexed formats may be indexed (INDEXABLE
// below): the ones LVGL draws as they are, and the clock's glass, which reads them through
// theme_art::Pixels. The clock's plate, its frames and the hands are copied and rotated pixel
// by pixel and stay as they were.
//
// Every indexed picture is unpacked again and compared with the original, pixel by pixel,
// before it is stored; a difference stores the original instead. The format can save space;
// it can never change what is drawn.
#include "theme_art.h"
#include "theme_font.h"
#include "theme_style.h"   // before PNGdec: its zlib defines `local`, which breaks lvgl's headers
#include "theme_sd.h"
#include "theme_select.h"
#include "orb_platform.h"

#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <new>
#include <string.h>

namespace theme_art {
namespace {

struct Asset { const char *name; bool alpha; bool indexable; };

// Most often shown first, as in src/theme_art_bake.cpp.
const Asset ASSETS[] = {
    { "menu_plate.png",        false, true  },
    { "settings_plate.png",    false, true  },
    { "radar_plate.png",       false, true  },
    { "clock_plate.png",       false, false },   // copied and rotated by clock_view.cpp
    { "clock_overlay.png",     true,  true  },   // the clock's glass reads Pixels
    { "clock_hand_hour.png",   true,  false },   // rotated pixel by pixel
    { "clock_hand_minute.png", true,  false },
    { "clock_hand_second.png", true,  false },
    { "radar_sweep.png",       true,  false },   // LVGL rotates it
    { "radar_blip.png",        true,  false },
    { "radar_static1.png",     true,  false },   // LVGL scales it
    { "radar_static2.png",     true,  false },
    { "clock_static1.png",     true,  false },   // drawn as a hand
    { "clock_static2.png",     true,  false },
    { "menu_overlay.png",      true,  true  },
    { "settings_overlay.png",  true,  true  },
    { "splash.png",            false, false },   // read raw by splash_art.cpp
    // Not in src/theme_art_bake.cpp's list, so on Arduino these were decoded off the card
    // (636 KB of PSRAM and ~0.2 s each) every time they were shown.
    { "splash_overlay.png",    true,  true  },   // LVGL, splash_lines.cpp
    { "radar_rings.png",       true,  true  },   // LVGL, radar_view.cpp
};
constexpr size_t SD_ASSET_MAX_BYTES = 2 * 1024 * 1024;

// Part of the fingerprint stored with a bake. Move it whenever the rules above change (an
// asset added, a format choice changed): every baked theme then bakes again on its next
// boot. Without it a theme whose own files did not change keeps the old bake for ever.
constexpr uint32_t BAKE_RULES = 2;   // 2: splash_overlay.png and radar_rings.png baked

void (*s_progress)(const char *, int, int) = nullptr;

PNG     *s_png   = nullptr;
uint8_t *s_buf   = nullptr;
int      s_w     = 0;
bool     s_alpha = false;

// Byte for byte the conversion src/theme_art_bake.cpp and custom_sprite.cpp use.
int line_cb(PNGDRAW *draw) {
    const uint8_t *src = draw->pPixels;
    const bool rgba = (draw->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA && draw->iBpp == 8);
    if (s_alpha) {
        uint8_t *dst = s_buf + (size_t)draw->y * s_w * 3;
        for (int x = 0; x < draw->iWidth; ++x, dst += 3) {
            if (rgba) { const uint16_t v = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3)); dst[0] = v & 0xFF; dst[1] = v >> 8; dst[2] = src[3]; src += 4; }
            else { dst[0] = dst[1] = dst[2] = 0; }
        }
    } else {
        uint16_t *dst = (uint16_t *)s_buf + (size_t)draw->y * s_w;
        for (int x = 0; x < draw->iWidth; ++x) {
            if (rgba) { dst[x] = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3)); src += 4; }
            else dst[x] = 0;
        }
    }
    return 1;
}

bool decode_png(const uint8_t *png, size_t len, bool alpha, uint8_t *&out, int &w, int &h) {
    if (!s_png) {
        void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!mem) return false;
        s_png = new (mem) PNG();
    }
    s_alpha = alpha;
    if (s_png->openRAM((uint8_t *)png, (int)len, line_cb) != PNG_SUCCESS) return false;
    w = s_png->getWidth();
    h = s_png->getHeight();
    out = (uint8_t *)heap_caps_malloc((size_t)w * h * (alpha ? 3 : 2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) { s_png->close(); return false; }
    s_buf = out;
    s_w = w;
    const int r = s_png->decode(nullptr, 0);
    s_png->close();
    if (r != PNG_SUCCESS) { heap_caps_free(out); out = nullptr; return false; }
    return true;
}

// A pixel as one number: RGB565 and alpha. Every fully clear pixel is the same key, because
// nothing draws its colour.
inline uint32_t key_at(const uint8_t *px, size_t i, bool alpha) {
    if (!alpha) return ((uint32_t)((const uint16_t *)px)[i] << 8) | 0xFF;
    const uint8_t a = px[i * 3 + 2];
    return a ? (((uint32_t)(px[i * 3] | (px[i * 3 + 1] << 8))) << 8) | a : 0;
}

// The palette under construction: an open-addressed table of keys (stored +1, so 0 is free).
struct Palette {
    static constexpr int SLOTS = 1024;
    uint32_t key[SLOTS];
    uint8_t  idx[SLOTS];
    uint32_t keys[256];
    int      n = 0;

    int find_or_add(uint32_t k) {
        uint32_t s = (k * 2654435761u) & (SLOTS - 1);
        while (key[s]) {
            if (key[s] == k + 1) return idx[s];
            s = (s + 1) & (SLOTS - 1);
        }
        if (n == 256) return -1;
        key[s] = k + 1;
        idx[s] = (uint8_t)n;
        keys[n] = k;
        return n++;
    }
};

// RGB565 widened to 8 bits a channel by repeating the top bits, so LVGL's narrowing (and
// Pixels') gives back exactly the RGB565 it came from.
inline void put_colour(uint8_t *c, uint32_t key) {
    const uint16_t v = (uint16_t)(key >> 8);
    const uint8_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
    c[0] = (uint8_t)((b << 3) | (b >> 2));
    c[1] = (uint8_t)((g << 2) | (g >> 4));
    c[2] = (uint8_t)((r << 3) | (r >> 2));
    c[3] = (uint8_t)(key & 0xFF);
}

// The picture as an indexed blob, or false when it has more than 256 colours (or memory ran
// out). On success `out` is a fresh PSRAM buffer the caller frees.
bool to_indexed(const uint8_t *px, int w, int h, bool alpha, uint8_t *&out, size_t &outLen, Format &fmt) {
    Palette *pal = (Palette *)heap_caps_calloc(1, sizeof(Palette), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pal) return false;
    const size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i)
        if (pal->find_or_add(key_at(px, i, alpha)) < 0) { heap_caps_free(pal); return false; }
    const int bpp = pal->n <= 2 ? 1 : pal->n <= 4 ? 2 : pal->n <= 16 ? 4 : 8;
    fmt = bpp == 1 ? FMT_INDEXED1 : bpp == 2 ? FMT_INDEXED2 : bpp == 4 ? FMT_INDEXED4 : FMT_INDEXED8;
    const size_t palBytes = (size_t)4 << bpp, stride = ((size_t)w * bpp + 7) / 8;
    outLen = palBytes + stride * h;
    out = (uint8_t *)heap_caps_calloc(1, outLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) { heap_caps_free(pal); return false; }
    for (int k = 0; k < pal->n; ++k) put_colour(out + k * 4, pal->keys[k]);
    uint8_t *rows = out + palBytes;
    for (int y = 0; y < h; ++y) {
        uint8_t *row = rows + y * stride;
        for (int x = 0; x < w; ++x) {
            const uint8_t k = (uint8_t)pal->find_or_add(key_at(px, (size_t)y * w + x, alpha));
            const int bit = x * bpp;
            row[bit >> 3] |= (uint8_t)(k << (8 - bpp - (bit & 7)));
        }
    }
    heap_caps_free(pal);

    // Unpack it again and compare, every pixel. A clear pixel matches any clear pixel.
    static Pixels check;
    check.set(out, w, h, fmt);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            const uint8_t k = check.index_at(x, y);
            const uint32_t want = key_at(px, i, alpha);
            const uint32_t got = check.palA[k] ? (((uint32_t)check.pal565[k]) << 8) | check.palA[k] : 0;
            if (got != want) {
                printf("[theme_art] indexed copy differs at %d,%d; storing the original\n", x, y);
                heap_caps_free(out);
                out = nullptr;
                return false;
            }
        }
    return true;
}

// Read, unpack, choose the format and store one picture. False when it did not make it into
// flash (it then stays on the SD path).
bool bake_picture(const char *slug, const char *name, bool alpha, bool indexable, bool &missed) {
    char path[80];
    snprintf(path, sizeof(path), "/themes/%s/%s", slug, name);
    size_t pngLen = 0;
    uint8_t *png = theme_sd::read_whole(path, pngLen, SD_ASSET_MAX_BYTES);
    if (!png) return false;
    uint8_t *raw = nullptr;
    int w = 0, h = 0;
    const bool ok = decode_png(png, pngLen, alpha, raw, w, h);
    theme_sd::free(png);
    if (!ok) { printf("[theme_art] %s: decode failed; it stays on the SD path\n", name); return false; }
    const size_t rawLen = (size_t)w * h * (alpha ? 3 : 2);
    uint8_t *blob = raw;
    size_t len = rawLen;
    Format fmt = alpha ? FMT_RGB565_ALPHA : FMT_RGB565;
    uint8_t *indexed = nullptr;
    if (indexable && to_indexed(raw, w, h, alpha, indexed, len, fmt)) blob = indexed;
    else len = rawLen;
    const bool stored = install_asset(slug, name, w, h, fmt, blob, len);
    if (stored) {
        if (indexed) printf("[theme_art] baked %-22s %dx%d %u KB, %d-bit indexed (was %u KB)\n",
                            name, w, h, (unsigned)(len / 1024), indexed_bpp(fmt), (unsigned)(rawLen / 1024));
        else         printf("[theme_art] baked %-22s %dx%d %u KB\n", name, w, h, (unsigned)(len / 1024));
    } else {
        missed = true;
    }
    if (indexed) heap_caps_free(indexed);
    heap_caps_free(raw);
    return stored;
}

}  // namespace

void set_progress(void (*cb)(const char *, int, int)) { s_progress = cb; }

bool bake_active_theme(bool force) {
    const char *slug = theme_select::activeSlug();
    if (!slug || !slug[0] || !space_total()) return false;
    // Re-bake when the theme's declared asset list moved, not only when nothing is baked.
    const uint32_t want = theme_style::assetsFingerprint() ^ (BAKE_RULES * 0x9E3779B9u);
    if (!force && slug_baked(slug) && baked_manifest(slug) == want && want != 0) {
        printf("[theme_art] '%s' already baked and unchanged\n", slug);
        return false;
    }
    printf("[theme_art] baking '%s' into flash (%s)\n", slug,
           force ? "forced, the whole partition" : slug_baked(slug) ? "its asset list changed" : "not baked yet");
    const uint32_t t0 = millis();
    if (!install_begin(slug, want, force)) { printf("[theme_art] install_begin failed; staying on SD\n"); return false; }

    size_t fontN = 0;
    const char *const *fonts = theme_font::slot_files(fontN);
    const int frames = theme_style::clock().bgAnim.frames;
    int total = 0;
    for (size_t i = 0; i < fontN; ++i) if (theme_style::hasAsset(fonts[i])) ++total;
    for (const Asset &a : ASSETS) if (theme_style::hasAsset(a.name)) ++total;
    char frameName[24];
    for (int f = 1; f <= frames; ++f) {
        snprintf(frameName, sizeof(frameName), "clock_plate_%02d.png", f);
        if (theme_style::hasAsset(frameName)) ++total;
    }
    if (s_progress) s_progress(nullptr, 0, total);
    int done = 0, baked = 0;
    bool missed = false;

    // Fonts first: small, and a font that lost the race would draw the compiled face.
    for (size_t i = 0; i < fontN; ++i) {
        if (!theme_style::hasAsset(fonts[i])) continue;
        if (s_progress) s_progress(fonts[i], ++done, total);
        char path[80];
        snprintf(path, sizeof(path), "/themes/%s/%s", slug, fonts[i]);
        size_t len = 0;
        uint8_t *buf = theme_sd::read_whole(path, len, SD_ASSET_MAX_BYTES);
        if (!buf) continue;
        if (install_asset(slug, fonts[i], 0, 0, FMT_RAW, buf, len)) {
            ++baked;
            printf("[theme_art] baked %-22s %u KB (font)\n", fonts[i], (unsigned)(len / 1024));
        } else {
            missed = true;
        }
        theme_sd::free(buf);
    }
    // Frames before the plates: a frame has no SD fallback, so losing the space race stops
    // the animation rather than slowing it (src/theme_art_bake.cpp tells that story).
    for (int f = 1; f <= frames; ++f) {
        snprintf(frameName, sizeof(frameName), "clock_plate_%02d.png", f);
        if (!theme_style::hasAsset(frameName)) continue;
        if (s_progress) s_progress(frameName, ++done, total);
        if (bake_picture(slug, frameName, false, false, missed)) ++baked;
    }
    for (const Asset &a : ASSETS) {
        if (!theme_style::hasAsset(a.name)) continue;   // never bake what the theme does not declare
        if (s_progress) s_progress(a.name, ++done, total);
        if (bake_picture(slug, a.name, a.alpha, a.indexable, missed)) ++baked;
    }

    // Something did not fit beside the other themes: once more, with the whole partition.
    if (missed && !force) {
        printf("[theme_art] something did not fit beside the other themes; baking again with "
               "the whole partition\n");
        return bake_active_theme(true);
    }
    if (!baked) { printf("[theme_art] nothing baked\n"); return false; }
    if (!install_commit()) { printf("[theme_art] commit failed; staying on SD\n"); return false; }
    printf("[theme_art] baked %d asset(s) in %u ms\n", baked, (unsigned)(millis() - t0));
    return true;
}

}  // namespace theme_art
