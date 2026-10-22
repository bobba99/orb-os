#pragma once
#include <stdint.h>
#include <stddef.h>

// Pre-baked theme art, resident in flash and drawn without any runtime work.
//
// The problem this solves, measured on a 466x466 plate (see docs/ARCHITECTURE.md):
//     read 342 KB PNG off the SD card   226 ms   (card is 20 MHz SPI, ~1.5 MB/s)
//     unpack the PNG to RGB565          205 ms
//                                       -------
//                                       431 ms, every single time the screen is shown,
//                                       plus 424 KB of PSRAM held while it is up.
//
// Both halves are avoidable. A PNG is a compressed picture; the panel wants raw RGB565.
// Nothing in the pipeline ever did that conversion ahead of time, so the device did it
// over and over. Launch Kit now bakes the exact bytes the panel wants at push time (on a
// Mac, where the work is free) and the device stores them in the `themeart` flash
// partition. The ESP32-S3 memory-maps flash through its cache, so an asset in there is
// just a pointer: no SD read, no decode, no PSRAM copy, no per-show cost at all.
//
// Deliberately NOT a replacement for the SD/PNG path. An asset that was never baked, or
// a theme installed before this existed, falls through to decode_sd_first() unchanged.
// Flash is a cache in front of that path, never a precondition for it.
namespace theme_art {

// Pixel layouts, matching custom_sprite.cpp's line_cb output byte for byte.
enum Format : uint8_t {
    FMT_RGB565       = 0,   // 2 bytes/px, opaque (plates, splash)
    FMT_RGB565_ALPHA = 1,   // 3 bytes/px, RGB565 + 8-bit alpha (overlays, hands)
    // Stored verbatim, not pixels. Fonts arrive as lv_font_conv binaries and are read
    // back through an lv_fs driver (theme_font.cpp), which is what lets a theme carry its
    // own typography instead of having it compiled into the firmware.
    FMT_RAW          = 2,
    // LVGL's indexed layouts, written only by the ESP-IDF store (components/orb_ui/themeart):
    // (1 << bpp) palette entries of 4 bytes each (B, G, R, A, LVGL's lv_color32_t), then
    // the rows, each starting on a byte, the leftmost pixel in the high bits. The palette
    // carries alpha, so one format covers an opaque plate with 85 colours (8 bits a pixel)
    // and a one-colour glass with one level of transparency (1 bit a pixel). The colours are
    // RGB565 widened by repeating their top bits, so narrowing them back is exact.
    FMT_INDEXED1     = 3,
    FMT_INDEXED2     = 4,
    FMT_INDEXED4     = 5,
    FMT_INDEXED8     = 6,
};

inline int indexed_bpp(Format f) {
    return f == FMT_INDEXED1 ? 1 : f == FMT_INDEXED2 ? 2 : f == FMT_INDEXED4 ? 4 : f == FMT_INDEXED8 ? 8 : 0;
}

// A picture as a compositor reads it, whatever it was stored as: RGB565+A8 pixels, or an
// indexed blob with its palette turned into RGB565 and alpha once, here, rather than per
// pixel. For code that mixes pixels itself (the clock's glass); LVGL draws the indexed
// formats on its own (theme_art_lv.h).
struct Pixels {
    Format         fmt = FMT_RGB565_ALPHA;
    int            w = 0, h = 0;
    const uint8_t *data = nullptr;   // RGB565+A8: the pixels. Indexed: the first row.
    int            bpp = 0, stride = 0;
    uint16_t       pal565[256] = {};
    uint8_t        palA[256] = {};

    bool set(const uint8_t *p, int width, int height, Format f) {
        fmt = f; w = width; h = height; data = p; bpp = indexed_bpp(f);
        if (!p) return false;
        if (!bpp) return f == FMT_RGB565_ALPHA;
        const int n = 1 << bpp;
        for (int i = 0; i < n; ++i) {
            const uint8_t *c = p + i * 4;   // B, G, R, A
            pal565[i] = (uint16_t)(((c[2] & 0xF8) << 8) | ((c[1] & 0xFC) << 3) | (c[0] >> 3));
            palA[i] = c[3];
        }
        data = p + n * 4;
        stride = (w * bpp + 7) / 8;
        return true;
    }
    uint8_t index_at(int x, int y) const {
        const uint8_t *row = data + (size_t)y * stride;
        switch (bpp) {
            case 8:  return row[x];
            case 4:  return (row[x >> 1] >> ((1 - (x & 1)) * 4)) & 0x0F;
            case 2:  return (row[x >> 2] >> ((3 - (x & 3)) * 2)) & 0x03;
            default: return (row[x >> 3] >> (7 - (x & 7))) & 0x01;
        }
    }
};

// Map the partition once at boot. Safe to call more than once; later calls are no-ops.
// Returns false when the partition is missing (older layout) or holds no valid index,
// which is not an error: every caller then just uses the SD path.
bool begin();

// Look up one baked asset for the active theme, e.g. "menu_plate.png".
// On success `out` points straight into memory-mapped flash and must NOT be freed.
// The pointer stays valid for the life of the process.
bool lookup(const char *slug, const char *assetName,
            const uint8_t *&out, int &w, int &h, Format &fmt);

// Convenience wrapper for the common "did this asset get baked?" question.
bool has(const char *slug, const char *assetName);

// The form every sprite file actually wants: look the asset up for the currently-active
// theme and confirm it is in the layout the caller is about to hand to LVGL. Returns
// nullptr on any miss, so the caller falls through to its existing SD/PNG path. The
// returned pointer is memory-mapped flash: never free it.
const uint8_t *find_active(const char *assetName, Format wantFmt, int &w, int &h);

// The same, for a caller that can draw an indexed blob as well as `wantFmt` (LVGL through
// theme_art_lv.h, or a compositor through Pixels). `fmt` says which it got.
const uint8_t *find_active_any(const char *assetName, Format wantFmt, int &w, int &h, Format &fmt);

// The format of a baked blob, by the pointer lookup() handed out. False for anything else
// (a decoded buffer in PSRAM), which a caller then describes the way it always has.
bool format_of(const void *p, Format &fmt);

// ---- install side (used by the /artput upload route) ----

// Start an install run for one theme. Entries belonging to OTHER themes are kept, so a
// second theme can be baked alongside the first and switching between them is free. Falls
// back to wiping everything when too little contiguous room is left.
// forgetOthers drops EVERY other theme's baked entries instead of keeping them, giving this
// bake the whole partition. Needed because nothing else ever reclaims the space: entries are
// only dropped for the slug being re-baked, so art belonging to a theme the owner deleted
// from the card sits in flash for ever, and the automatic clean-slate below only fires once
// the remainder has already fallen under FULL_THEME_BYTES. A rich theme that does not fit
// alongside a stale one loses whatever is baked LAST, which is the animation frames.
bool install_begin(const char *slug, uint32_t manifestFingerprint, bool forgetOthers = false);
// Append one baked asset. `data` is raw pixels, already in `fmt` layout.
bool install_asset(const char *slug, const char *assetName,
                   int w, int h, Format fmt, const uint8_t *data, size_t len);
// Write the index and re-map. Nothing is visible to lookup() until this succeeds.
bool install_commit();

// Bytes free for further install_asset() calls.
size_t space_free();
// Total usable bytes in the partition (0 when the partition is absent).
size_t space_total();

// Raw bytes of a baked asset, whatever its format. Used for FMT_RAW blobs (fonts),
// where there is nothing to interpret and the caller just wants a pointer and a length.
bool find_blob(const char *slug, const char *assetName, const uint8_t *&data, size_t &len);

// Does this pointer aim into the memory-mapped partition? Release paths use this instead
// of tracking a per-asset "came from flash" flag: a buffer that theme_art owns was never
// allocated, so freeing it would be a wild pointer into flash.
bool owns(const void *p);

// The asset-list fingerprint stored with this theme's bake, or 0 if it is not baked.
// Compared against theme_style::assetsFingerprint() to decide whether a re-bake is due.
// Per theme, not per partition: several themes stay baked at once, and each has to be
// able to go stale on its own.
uint32_t baked_manifest(const char *slug);

// Does the cache already hold anything for this theme?
bool slug_baked(const char *slug);

// ESP-IDF store only (components/orb_ui/themeart): drop the art of every theme whose slug is
// not among `slugs` (the themes on the card), freeing its room. Returns how many entries
// went, or -1 if the index could not be rewritten. The caller makes sure the card is there.
int reclaim_missing(const char *const *slugs, int n);

// One-time conversion for the active theme: read each PNG off the card, unpack it once,
// and store the raw pixels in flash. Runs at boot only when the active theme is not
// already baked, i.e. right after a theme push, inside the reboot the user is already
// waiting through. No-op once done, and a failure anywhere just leaves that asset on the
// SD path. Returns true if anything was baked.
// force: bake even when the fingerprint says nothing changed, and give the bake the whole
// partition (see install_begin's forgetOthers). This is the "it did not all fit, try again
// with room" path, exposed as /rebake.
bool bake_active_theme(bool force = false);

// Progress hook for the bake, called from the same task that runs it. First call is
// (nullptr, 0, total) meaning "starting"; then (assetName, done, total) per asset. Set by
// main so the boot screen can narrate the install; never required.
void set_progress(void (*cb)(const char *assetName, int done, int total));

} // namespace theme_art
