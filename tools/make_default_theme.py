#!/usr/bin/env python3
"""The Orb OS — write the default theme, themes/orb-plain/, from nothing but this script.

    python3 tools/make_default_theme.py            # writes themes/orb-plain/

An original, deliberately plain theme: a dark dial with minute and hour marks, three slim
hands and a hub, and quiet plates for the menu, Settings and the Flight Tracker. It exists so
the repository carries a theme it owns outright (MIT, like the rest), that shows the format
(docs/themes.md) and that a fresh card can be given. Every picture is drawn here from
numbers; the JSON files are written from the same numbers, so a hand's pivot can never
disagree with its picture.

Shapes are anti-aliased from their distance to the edge, one pixel wide, so the dial has
some tens of colours and the baker stores the plates as 8-bit indexed images.
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orbpng  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SLUG = "orb-plain"
S = 466
C = (S - 1) / 2.0                     # the centre, between pixels 232 and 233

BG     = (11, 14, 18)
RING   = (40, 46, 54)
MINUTE = (92, 100, 110)
HOUR   = (222, 228, 234)
HANDS  = (232, 236, 240)
ACCENT = (255, 106, 61)
DATE   = 0xA0A8B2


def cover(d):
    """How much of a pixel lies inside a shape whose edge is d pixels away (inside < 0)."""
    return max(0.0, min(1.0, 0.5 - d))


def blend(dst, src, a):
    return tuple(round(d + (s - d) * a) for d, s in zip(dst, src))


def canvas(colour, alpha=255):
    return [[(colour[0], colour[1], colour[2], alpha) for _ in range(S)] for _ in range(S)]


def put(img, x, y, colour, a):
    if a <= 0:
        return
    r, g, b, da = img[y][x]
    if da == 0:                       # onto clear: the colour, at this coverage
        img[y][x] = (colour[0], colour[1], colour[2], round(255 * a))
    else:                             # onto opaque: mixed
        img[y][x] = blend((r, g, b), colour, a) + (da,)


def ring(img, radius, width, colour):
    for y in range(S):
        for x in range(S):
            d = abs(math.hypot(x - C, y - C) - radius) - width / 2.0
            put(img, x, y, colour, cover(d))


def ticks(img, count, r0, r1, width, colour, skip_every=0):
    """`count` marks from radius r0 to r1, `width` px wide; every `skip_every`th left out."""
    step = 2 * math.pi / count
    for y in range(S):
        for x in range(S):
            dx, dy = x - C, y - C
            r = math.hypot(dx, dy)
            if r < r0 - 2 or r > r1 + 2:
                continue
            ang = math.atan2(dx, -dy) % (2 * math.pi)        # 0 at twelve, clockwise
            k = round(ang / step) % count
            if skip_every and k % skip_every == 0:
                continue
            across = abs(r * math.sin(ang - k * step)) - width / 2.0
            along = max(r0 - r, r - r1)
            put(img, x, y, colour, cover(max(across, along)))


def flatten(img):
    return bytes(v for row in img for px in row for v in px)


def write(name, img, w=S, h=S):
    orbpng.write_rgba(os.path.join(OUT, name), w, h, flatten(img))
    print("  " + name)


def hand(name, width, length, tail, colour):
    """A capsule pointing up, its pivot `length` px below the tip. Returns the pivot."""
    pad = 2
    w = int(math.ceil(width)) + 2 * pad
    h = length + tail + 2 * pad
    px, py = (w - 1) / 2.0, pad + length          # the pivot, in the picture's pixels
    img = [[(colour[0], colour[1], colour[2], 0) for _ in range(w)] for _ in range(h)]
    top, bottom = py - length + width / 2.0, py + tail - width / 2.0
    for y in range(h):
        for x in range(w):
            cy = min(max(y, top), bottom)              # nearest point on the centre line
            d = math.hypot(x - px, y - cy) - width / 2.0
            a = cover(d)
            if a > 0:
                img[y][x] = (colour[0], colour[1], colour[2], round(255 * a))
    orbpng.write_rgba(os.path.join(OUT, name), w, h, flatten(img))
    print("  " + name)
    return round(px), round(py)


def main():
    os.makedirs(OUT, exist_ok=True)
    assets = []

    # The dial.
    img = canvas(BG)
    ring(img, 229, 2, RING)
    ticks(img, 60, 214, 224, 2, MINUTE, skip_every=5)
    ticks(img, 12, 194, 224, 6, HOUR)
    write("clock_plate.png", img)
    assets.append("clock_plate.png")

    # The hub over the hands: the clock's glass. Clear everywhere else.
    img = canvas(BG, 0)
    for y in range(S):
        for x in range(S):
            r = math.hypot(x - C, y - C)
            put(img, x, y, HANDS, cover(r - 7))
            put(img, x, y, ACCENT, cover(r - 3))
    write("clock_overlay.png", img)
    assets.append("clock_overlay.png")

    hands = {}
    for key, name, width, length, tail, colour in (
            ("hour", "clock_hand_hour.png", 10, 130, 18, HANDS),
            ("minute", "clock_hand_minute.png", 6, 196, 22, HANDS),
            ("second", "clock_hand_second.png", 2, 206, 36, ACCENT)):
        pxy = hand(name, width, length, tail, colour)
        hands[key] = {"show": True, "pivotX": pxy[0], "pivotY": pxy[1], "centerX": 233, "centerY": 233}
        assets.append(name)
    hands["order"] = [0, 1, 2]

    # The other screens: the dial's ground and its outer ring, nothing that competes with
    # what they draw on top.
    img = canvas(BG)
    ring(img, 229, 2, RING)
    for name in ("menu_plate.png", "settings_plate.png", "radar_plate.png"):
        write(name, img)
        assets.append(name)

    theme = {
        "schema": 1,
        "name": "Plain",
        "author": "The Orb OS",
        "apps": {"clock": True, "flight": True, "headlines": True},
        "assets": assets,
    }
    clock = {
        "bg": 0x0B0E12,
        "secondSweep": True,
        "hands": hands,
        "text1": {"show": True, "x": 233, "y": 300, "align": 1, "color": DATE, "fmt": "%a %d"},
        "text2": {"show": False},
    }
    # A theme that says nothing about a screen gets the compiled design's look for it, so the
    # colours of every screen with text or traffic on it are stated here.
    hexc = lambda c: (c[0] << 16) | (c[1] << 8) | c[2]   # noqa: E731
    menu = {
        "current": {"color": hexc(HOUR), "glow": 0, "opa": 255},
        "prev": {"color": hexc(MINUTE), "glow": 0, "opa": 255},
        "next": {"color": hexc(MINUTE), "glow": 0, "opa": 255},
    }
    settings = {
        "itemColor": 0x9AA2AC, "itemGlow": 0, "selColor": hexc(HOUR), "selGlow": 0,
        "glow": 0, "hlShow": False,
    }
    radar = {
        "sweepEnabled": True, "sweepTypeImage": False, "sweepColor": hexc(ACCENT),
        "sweepLeadColor": 0xFFD2C2, "sweepOpacity": 40, "sweepHubOn": False,
        "blipEnabled": True, "blipTypeImage": False, "blipKiteShape": True, "blipSize": 9,
        "blipFixedColorMode": True, "blipFixedColor": hexc(HANDS), "blipGlow": 0,
        "centerEnabled": True, "centerColor": hexc(ACCENT), "centerInnerColor": hexc(BG),
        "selColor": hexc(ACCENT),
    }
    for name, doc in (("theme.json", theme), ("clock_style.json", clock), ("menu_style.json", menu),
                      ("settings_style.json", settings), ("radar_style.json", radar)):
        with open(os.path.join(OUT, name), "w") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
        print("  " + name)
    # theme_select lists only folders carrying this marker: a finished theme.
    open(os.path.join(OUT, "_installed"), "w").close()
    print("  _installed")


OUT = os.path.join(ROOT, "themes", SLUG)

if __name__ == "__main__":
    print("writing " + os.path.relpath(OUT, ROOT))
    main()
