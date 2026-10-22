#!/usr/bin/env python3
"""The Orb OS — regenerate the built-in fonts with Norwegian letters and real punctuation.

    python3 tools/gen_fonts.py

Stage 6 of REWRITE_PLAN.md. Every built-in font covered only 0x20-0x7F, so æ, ø, å, é, «»,
dashes, curly quotes and … drew as empty boxes or nothing. This writes them again with

    0x20-0x7F, 0xA0-0xFF (Latin-1: æøåÆØÅ, é, «», °, ·), 0x2013 0x2014 (– —),
    0x2018 0x2019 0x201C 0x201D (‘ ’ “ ”), 0x2022 (•), 0x2026 (…)

and changes nothing else:

  - LVGL's Montserrat, every size lv_conf.h turns on, from LVGL's own source files and with
    LVGL's own recorded options (its icon glyphs included), into src/fonts/ under LVGL's
    names. lv_conf.h turns LVGL's copies off and declares these, so no call site changes.
    The same options without the new range reproduce LVGL's files byte for byte.
  - Inter (the splash's standing lines), src/font_inter_*.c, from tools/fonts/InterVariable.ttf
    (Inter 4.1, SIL Open Font Licence, tools/fonts/OFL-Inter.txt), the file that reproduces the
    old ASCII-only fonts' metrics exactly.

Needs node; runs lv_font_conv 1.5.2 (the version LVGL's files name) through npx.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RANGE = "0x20-0x7F,0xA0-0xFF,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0x2022,0x2026"
CONV = ["npx", "--yes", "lv_font_conv@1.5.2"]
INTER = os.path.join(ROOT, "tools", "fonts", "InterVariable.ttf")
INTER_SIZES = [12, 14, 16, 18, 20]


def lvgl_dir():
    """LVGL as the ESP-IDF build or the simulator build downloaded it."""
    for d in ("managed_components/lvgl__lvgl", ".pio/libdeps/native/lvgl",
              ".pio/libdeps/esp32-s3-amoled-175/lvgl"):
        if os.path.exists(os.path.join(ROOT, d, "scripts", "built_in_font", "Montserrat-Medium.ttf")):
            return os.path.join(ROOT, d)
    sys.exit("LVGL's font sources not found: build once (idf.py build or pio run -e native) first")


def montserrat_sizes():
    conf = open(os.path.join(ROOT, "include", "lv_conf.h")).read()
    return sorted(int(n) for n in re.findall(r"ORB_FONT_MONTSERRAT_(\d+)\s+1", conf))


def always_built(path, guard):
    """lv_font_conv guards a font with a macro named after it, LV_FONT_MONTSERRAT_28 for
    lv_font_montserrat_28, which lv_conf.h sets to 0 to drop LVGL's own copy. Ours must not
    share that switch."""
    s = open(path).read()
    s = re.sub(r"#ifndef %s\n#define %s 1\n#endif\n\n" % (guard, guard), "", s)
    s = s.replace("#endif /*#if %s*/" % guard, "#endif")   # first: it contains the next one
    s = s.replace("#if %s" % guard, "#if 1   /* tools/gen_fonts.py: always built */")
    open(path, "w").write(s)


def run(args, cwd):
    subprocess.run(CONV + args, cwd=cwd, check=True, stdout=subprocess.DEVNULL)


def main():
    lv = lvgl_dir()
    src_fonts = os.path.join(lv, "scripts", "built_in_font")
    out_dir = os.path.join(ROOT, "src", "fonts")
    os.makedirs(out_dir, exist_ok=True)
    for size in montserrat_sizes():
        name = "lv_font_montserrat_%d" % size
        original = open(os.path.join(lv, "src", "font", name + ".c")).read()
        opts = re.search(r"\* Opts: (.*)", original).group(1).split()
        # LVGL's options, with the text range widened and the output moved; nothing else.
        first_range = opts.index("-r")
        assert opts[first_range + 1] == "0x20-0x7F,0xB0,0x2022", opts[first_range + 1]
        opts[first_range + 1] = RANGE
        o = opts.index("-o")
        out = os.path.join(out_dir, name + ".c")
        opts[o + 1] = out
        run(opts + ["--lv-include", "lvgl.h"], cwd=src_fonts)
        always_built(out, name.upper())
        print("  src/fonts/%s.c" % name)
    for size in INTER_SIZES:
        name = "font_inter_%d" % size
        out = os.path.join(ROOT, "src", name + ".c")
        run(["--font", INTER, "-r", RANGE, "--size", str(size), "--bpp", "4", "--format", "lvgl",
             "--no-compress", "--lv-include", "lvgl.h", "-o", out], cwd=ROOT)
        print("  src/%s.c" % name)


if __name__ == "__main__":
    main()
