#pragma once
// LVGL's side of theme_art: image descriptions for baked art in any of its formats.
#include <lvgl.h>
#include "theme_art.h"

namespace theme_art {

inline lv_img_cf_t lv_cf(Format f) {
    switch (f) {
        case FMT_RGB565:       return LV_IMG_CF_TRUE_COLOR;
        case FMT_RGB565_ALPHA: return LV_IMG_CF_TRUE_COLOR_ALPHA;
        case FMT_INDEXED1:     return LV_IMG_CF_INDEXED_1BIT;
        case FMT_INDEXED2:     return LV_IMG_CF_INDEXED_2BIT;
        case FMT_INDEXED4:     return LV_IMG_CF_INDEXED_4BIT;
        case FMT_INDEXED8:     return LV_IMG_CF_INDEXED_8BIT;
        default:               return LV_IMG_CF_UNKNOWN;
    }
}

inline uint32_t blob_bytes(Format f, int w, int h) {
    if (const int bpp = indexed_bpp(f)) return (uint32_t)(4u << bpp) + (uint32_t)((w * bpp + 7) / 8) * h;
    return (uint32_t)w * h * (f == FMT_RGB565_ALPHA ? 3 : 2);
}

// Call after filling in a description the way the caller always has. When its pixels are a
// baked blob, the format and size are made to match what was baked; anything else (pixels
// decoded into PSRAM) is left exactly as it was. Every description of baked art goes
// through this, so a picture the baker stores in a new format cannot be drawn as the old one.
inline void adopt(lv_img_dsc_t &d) {
    Format f;
    if (!format_of(d.data, f)) return;
    d.header.cf = lv_cf(f);
    d.data_size = blob_bytes(f, d.header.w, d.header.h);
}

}  // namespace theme_art
