#pragma once
// UTF-8 for the hand-drawn text paths.
//
// Several screens draw their own glyphs out of a font (curved_text, menu_text,
// settings_text, the clock's banners) instead of handing a string to an LVGL label. They
// walked the string a byte at a time, which was harmless while every built-in font held only
// ASCII: the two bytes of "ø" found no glyph and drew nothing. Stage 6 gave the fonts
// Latin-1, and each of those bytes then found a glyph of its own, so "Tromsø" drew as
// "TromsÃ¸". Every such path decodes through here instead, so a glyph is looked up by the
// character it is, not by a byte of it.
#include <stdint.h>

// One code point out of UTF-8, advancing p; U+FFFD for a broken sequence, which no font
// carries and so draws nothing.
inline uint32_t orb_utf8_next(const unsigned char *&p) {
    const unsigned char c = *p++;
    if (c < 0x80) return c;
    const int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (!more) return 0xFFFD;
    uint32_t cp = more == 3 ? (c & 0x07u) : more == 2 ? (c & 0x0Fu) : (c & 0x1Fu);
    for (int k = 0; k < more; ++k) {
        if ((*p & 0xC0) != 0x80) return 0xFFFD;   // stops on the NUL too: never reads past it
        cp = (cp << 6) | (*p++ & 0x3Fu);
    }
    return cp;
}

// The first `max` characters of `s` (bytes [s, end) when end is given) as code points.
// Returns how many were written.
inline int orb_utf8_decode(const char *s, uint32_t *out, int max, const char *end = nullptr) {
    const unsigned char *p = (const unsigned char *)s, *e = (const unsigned char *)end;
    int n = 0;
    while (p && *p && n < max && (!e || p < e)) out[n++] = orb_utf8_next(p);
    return n;
}

// One code point as UTF-8 in `out` (room for 5), NUL-terminated: for code that hands a single
// character to LVGL as a string.
inline void orb_utf8_encode(uint32_t cp, char out[5]) {
    unsigned char *o = (unsigned char *)out;
    if (cp < 0x80)         { *o++ = (unsigned char)cp; }
    else if (cp < 0x800)   { *o++ = (unsigned char)(0xC0 | (cp >> 6)); *o++ = (unsigned char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { *o++ = (unsigned char)(0xE0 | (cp >> 12)); *o++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                             *o++ = (unsigned char)(0x80 | (cp & 0x3F)); }
    else                   { *o++ = (unsigned char)(0xF0 | (cp >> 18)); *o++ = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                             *o++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (unsigned char)(0x80 | (cp & 0x3F)); }
    *o = 0;
}
