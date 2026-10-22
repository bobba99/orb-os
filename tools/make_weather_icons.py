#!/usr/bin/env python3
"""The Orb OS — draw the Weather app's icons into src/wx_icons_data.c.

    python3 tools/make_weather_icons.py            # writes src/wx_icons_data.c
    python3 tools/make_weather_icons.py --preview  # also sim/shots/wx_icons.png

Twelve plain symbols, drawn here from distance fields so their edges are anti-aliased at any
size, and stored as LVGL 8-bit alpha images: a shape and no colour, so the Weather screen
paints them in the theme's icon colour. MET Norway's ~40 symbol codes map onto them in
src/wx_icons.cpp. Original work, under this repository's licence.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = {"big": 96, "small": 44}


# ---- signed distance primitives, in a unit box (0..1), negative inside ----------------------

def circle(x, y, cx, cy, r):
    return math.hypot(x - cx, y - cy) - r


def capsule(x, y, ax, ay, bx, by, r):
    px, py, dx, dy = x - ax, y - ay, bx - ax, by - ay
    t = max(0.0, min(1.0, (px * dx + py * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - dx * t, py - dy * t) - r


def polygon(x, y, pts):
    d = min(capsule(x, y, *pts[i], *pts[(i + 1) % len(pts)], 0.0) for i in range(len(pts)))
    inside = False
    for i in range(len(pts)):
        (x1, y1), (x2, y2) = pts[i], pts[(i + 1) % len(pts)]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            inside = not inside
    return -d if inside else d


def cloud(x, y, ox=0.0, oy=0.0, s=1.0):
    """A cloud: three bumps on a flat-bottomed body."""
    x, y = (x - ox) / s, (y - oy) / s
    d = min(circle(x, y, 0.36, 0.52, 0.16), circle(x, y, 0.55, 0.42, 0.21),
            circle(x, y, 0.72, 0.55, 0.13), capsule(x, y, 0.33, 0.62, 0.73, 0.62, 0.06))
    return d * s


def sun(x, y, cx=0.5, cy=0.5, s=1.0):
    d = circle(x, y, cx, cy, 0.17 * s)
    for k in range(8):
        a = k * math.pi / 4
        d = min(d, capsule(x, y, cx + math.cos(a) * 0.27 * s, cy + math.sin(a) * 0.27 * s,
                           cx + math.cos(a) * 0.37 * s, cy + math.sin(a) * 0.37 * s, 0.028 * s))
    return d


def moon(x, y, cx=0.5, cy=0.5, s=1.0):
    return max(circle(x, y, cx, cy, 0.26 * s), -circle(x, y, cx + 0.13 * s, cy - 0.09 * s, 0.22 * s))


def drops(x, y, n):
    """n short slanted strokes under the cloud."""
    d = 1e9
    xs = {2: (0.42, 0.62), 3: (0.34, 0.52, 0.70), 4: (0.30, 0.44, 0.58, 0.72)}[n]
    for i, px in enumerate(xs):
        top = 0.70 + (0.04 if i % 2 else 0.0)
        d = min(d, capsule(x, y, px + 0.04, top, px - 0.04, top + 0.18, 0.032))
    return d


def flake(x, y, cx, cy, r):
    d = 1e9
    for k in range(3):
        a = k * math.pi / 3
        d = min(d, capsule(x, y, cx - math.cos(a) * r, cy - math.sin(a) * r,
                           cx + math.cos(a) * r, cy + math.sin(a) * r, 0.024))
    return d


GAP = 0.035   # the clear ring a cloud cuts out of what is behind it


def behind(front, back):
    """`back` with a clear margin where `front` covers it, then `front` on top."""
    return min(front, max(back, -(front - GAP)))


GLYPHS = {
    "sun":        lambda x, y: sun(x, y),
    "moon":       lambda x, y: moon(x, y),
    "fair_day":   lambda x, y: behind(cloud(x, y, 0.06, 0.12, 0.92), sun(x, y, 0.36, 0.34, 0.78)),
    "fair_night": lambda x, y: behind(cloud(x, y, 0.06, 0.12, 0.92), moon(x, y, 0.36, 0.34, 0.80)),
    "cloud":      lambda x, y: cloud(x, y, 0.0, 0.02),
    "fog":        lambda x, y: min(capsule(x, y, 0.20, 0.36, 0.80, 0.36, 0.04),
                                   capsule(x, y, 0.14, 0.52, 0.74, 0.52, 0.04),
                                   capsule(x, y, 0.26, 0.68, 0.86, 0.68, 0.04)),
    "rain_light": lambda x, y: min(cloud(x, y, 0.0, -0.16), drops(x, y, 2)),
    "rain":       lambda x, y: min(cloud(x, y, 0.0, -0.16), drops(x, y, 3)),
    "rain_heavy": lambda x, y: min(cloud(x, y, 0.0, -0.16), drops(x, y, 4)),
    "sleet":      lambda x, y: min(cloud(x, y, 0.0, -0.16),
                                   capsule(x, y, 0.40, 0.70, 0.34, 0.88, 0.032), flake(x, y, 0.62, 0.80, 0.09)),
    "snow":       lambda x, y: min(cloud(x, y, 0.0, -0.16), flake(x, y, 0.32, 0.78, 0.08),
                                   flake(x, y, 0.54, 0.88, 0.08), flake(x, y, 0.74, 0.76, 0.08)),
    "thunder":    lambda x, y: min(cloud(x, y, 0.0, -0.16),
                                   polygon(x, y, [(0.56, 0.58), (0.38, 0.82), (0.50, 0.82),
                                                  (0.42, 1.00), (0.66, 0.74), (0.53, 0.74), (0.62, 0.58)])),
}


def render(fn, size):
    """8-bit alpha, row by row: coverage from the distance at each pixel's centre."""
    out = bytearray(size * size)
    for j in range(size):
        y = (j + 0.5) / size
        for i in range(size):
            d = fn((i + 0.5) / size, y) * size          # in pixels
            out[j * size + i] = round(255 * max(0.0, min(1.0, 0.5 - d)))
    return out


def main():
    lines = [
        "// Generated by tools/make_weather_icons.py. Do not edit; run the script.",
        "// The Weather app's icons: shapes with no colour (LVGL 8-bit alpha), painted in the",
        "// theme's icon colour. Original work, under this repository's licence.",
        "#include <lvgl.h>", "",
    ]
    previews = []
    for name, fn in GLYPHS.items():
        for tag, size in SIZES.items():
            a = render(fn, size)
            ident = "wx_%s_%s" % (name, tag)
            lines.append("static const uint8_t %s_px[%d] = {" % (ident, len(a)))
            for k in range(0, len(a), 24):
                lines.append("    " + ",".join("0x%02X" % v for v in a[k:k + 24]) + ",")
            lines.append("};")
            lines.append("const lv_img_dsc_t %s = { { LV_IMG_CF_ALPHA_8BIT, 0, 0, %d, %d }, %d, %s_px };"
                         % (ident, size, size, len(a), ident))
            lines.append("")
            if tag == "big":
                previews.append(a)
    path = os.path.join(ROOT, "src", "wx_icons_data.c")
    open(path, "w").write("\n".join(lines))
    print("wrote src/wx_icons_data.c: %d icons at %s px" % (len(GLYPHS), ", ".join(str(s) for s in SIZES.values())))

    if "--preview" in sys.argv:
        import orbpng
        s, cols = SIZES["big"], 6
        rows = (len(previews) + cols - 1) // cols
        w, h = cols * s, rows * s
        rgba = bytearray(w * h * 4)
        for k, a in enumerate(previews):
            ox, oy = (k % cols) * s, (k // cols) * s
            for j in range(s):
                for i in range(s):
                    v = a[j * s + i]
                    p = ((oy + j) * w + ox + i) * 4
                    rgba[p:p + 4] = bytes((round(11 + (230 - 11) * v / 255), round(14 + (232 - 14) * v / 255),
                                           round(18 + (236 - 18) * v / 255), 255))
        os.makedirs(os.path.join(ROOT, "sim", "shots"), exist_ok=True)
        orbpng.write_rgba(os.path.join(ROOT, "sim", "shots", "wx_icons.png"), w, h, rgba)
        print("preview: sim/shots/wx_icons.png")


if __name__ == "__main__":
    main()
