#pragma once
// Fit a line of text to the font that will draw it.
//
// Text from the network (a flight's operator, a route, a place name) can hold any character,
// and a font only some: LVGL draws a missing glyph as nothing or as a box. This keeps every
// character the font has, turns an accented letter it lacks into its plain letter when it has
// that ("é" to "e" in a font of plain ASCII), and leaves out the rest. It asks the font itself,
// so a font that gains letters (tools/gen_fonts.py gave the built-in ones æøå and the rest of
// Latin-1) shows them with no change here, and one a theme brings is held to what it carries.
#include <lvgl.h>
#include <stdint.h>
#include "orb_utf8.h"   // orb_utf8_next()

// The plain letter under a Latin-1 accented one, or 0.
inline char orb_fold_latin1(uint32_t cp) {
    static const char *const MAP =   // U+00C0 .. U+00FF; '\0' where there is no plain letter
        "AAAAAAACEEEEIIII" "DNOOOOO\0OUUUUYTs"
        "aaaaaaaceeeeiiii" "dnooooo\0ouuuuyty";
    return (cp >= 0xC0 && cp <= 0xFF) ? MAP[cp - 0xC0] : 0;
}

// Does the font (or one it falls back to) have this character? Blank glyphs count: a no-break
// space has nothing to draw and is still a character.
inline bool orb_font_has(const lv_font_t *font, uint32_t cp) {
    lv_font_glyph_dsc_t g;
    return lv_font_get_glyph_dsc(font, &g, cp, 0);
}

// In place. A null font keeps everything.
inline void orb_text_fit(char *s, const lv_font_t *font) {
    if (!s || !font) return;
    const unsigned char *p = (const unsigned char *)s;
    char *o = s;
    while (*p) {
        const unsigned char *start = p;
        const uint32_t cp = orb_utf8_next(p);
        if (cp < 0x80 || orb_font_has(font, cp)) {
            while (start < p) *o++ = (char)*start++;      // as it was
        } else if (const char plain = orb_fold_latin1(cp)) {
            if (orb_font_has(font, (uint32_t)(unsigned char)plain)) *o++ = plain;
        }
    }
    *o = 0;
}
