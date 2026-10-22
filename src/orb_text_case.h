#pragma once
// ALL CAPS, for any line of text a theme draws.
//
// Zion asked for it on every text block on every screen (2026-09-23), including the ones
// nobody can retype: the firmware version and the network address on the splash screen are
// written by the device, not by the designer, so a design that wants small caps across its
// face could not have them there at any price.
//
// Applied to the FINISHED string, at the moment of drawing, rather than to the format the
// designer typed. That is the only place it can work for all of them at once: "%A" becomes
// Wednesday inside strftime, "{callsign}" becomes UAL328 inside the token expander, and the
// version line is built from a compiled constant. Uppercasing the recipe would miss every
// one of those.
//
// ASCII and Latin-1. The built-in faces carry Latin-1 since stage 6 (tools/gen_fonts.py), so
// "Tromsø" becomes "TROMSØ"; æ, ø, å and the accented vowels have their capitals in the same
// block, two bytes each in UTF-8 with only the second byte moving. ÿ, whose capital lives
// outside Latin-1, and every other character are left exactly as they were. A theme font
// that lacks a capital draws nothing for it; tools/theme_tool.py validate warns about that.
#include <ctype.h>
#include <stdio.h>
#include <stddef.h>

inline void orb_upper(char *s) {
    if (!s) return;
    for (; *s; ++s) {
        const unsigned char c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z') *s = (char)(c - 32);
        // UTF-8 C3 A0..BE is U+00E0..U+00FE; its capital is 0x20 lower, except ÷ (C3 B7).
        else if (c == 0xC3 && s[1]) {
            const unsigned char d = (unsigned char)s[1];
            if (d >= 0xA0 && d <= 0xBE && d != 0xB7) s[1] = (char)(d - 0x20);
            ++s;
        }
    }
}

// The same thing, for the draw sites that hold a const string and a buffer they own.
inline const char *orb_upper_into(char *buf, size_t n, const char *src, bool on) {
    if (!on || !src) return src;
    snprintf(buf, n, "%s", src);
    orb_upper(buf);
    return buf;
}
