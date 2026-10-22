#!/usr/bin/env python3
"""The Orb OS — check, size and preview a theme folder on a computer.

    python3 tools/theme_tool.py validate themes/orb-plain
    python3 tools/theme_tool.py estimate themes/orb-plain
    python3 tools/theme_tool.py preview  themes/orb-plain      # simulator screenshots
    python3 tools/theme_tool.py keys                           # what each file may say
    python3 tools/theme_tool.py serve themes/orb-plain [more]  # for a dev build to download

The rules are docs/themes.md's. `validate` checks what main/theme_check.cpp checks on the Orb,
and more that only a computer has time for. `estimate` follows the baker
(components/orb_ui/themeart/bake.cpp): which pictures it stores, which it may store indexed,
and how; the number it prints is what the Orb will report when it bakes the theme. `keys`
reads the loader (src/theme_style.cpp) itself, so it cannot fall behind it.

Plain Python 3, no packages.
"""
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orbpng  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA = 1
STYLE_MAX = 8192                  # theme_style.cpp MAX_STYLE_JSON_BYTES
SLUG_MAX = 31                     # theme_select.h MAX_SLUG_LEN, less the terminator
SCREEN = 466
PARTITION = 0x9A0000 - 8192       # the themeart partition, less its index
SECTOR = 4096
STYLE_FILES = ["clock_style.json", "radar_style.json", "settings_style.json", "menu_style.json",
               "splash_style.json", "intel_style.json", "ticker_style.json", "weather_style.json"]

# bake.cpp's ASSETS, in its order: (name, has alpha, may be indexed).
BAKED = [
    ("menu_plate.png", False, True), ("settings_plate.png", False, True),
    ("radar_plate.png", False, True), ("clock_plate.png", False, False),
    ("clock_overlay.png", True, True), ("clock_hand_hour.png", True, False),
    ("clock_hand_minute.png", True, False), ("clock_hand_second.png", True, False),
    ("radar_sweep.png", True, False), ("radar_blip.png", True, False),
    ("radar_static1.png", True, False), ("radar_static2.png", True, False),
    ("clock_static1.png", True, False), ("clock_static2.png", True, False),
    ("menu_overlay.png", True, True), ("settings_overlay.png", True, True),
    ("splash.png", False, False), ("splash_overlay.png", True, True),
    ("radar_rings.png", True, True), ("radar_overlay.png", True, True),
    ("clock_shadow_hour.png", True, False), ("clock_shadow_minute.png", True, False),
    ("clock_shadow_second.png", True, False), ("wind_bg.png", False, False),
    ("wind_crank.png", True, False), ("wind_crank_shadow.png", True, False),
]
# Read off the card when shown, never baked.
CARD_ONLY = ["intel_plate.png", "intel_overlay.png", "radar_card.png", "weather_plate.png",
             "ticker_plate.png"]
FULL_SCREEN = {n for n, _, _ in BAKED if n.endswith(("_plate.png", "_overlay.png"))} | {
    "splash.png", "radar_rings.png", "wind_bg.png"} | {n for n in CARD_ONLY if "plate" in n or "overlay" in n}


def font_files():
    """theme_font.cpp's slot table: the font binaries a theme may ship."""
    src = open(os.path.join(ROOT, "src", "theme_font.cpp")).read()
    return sorted(set(re.findall(r'"(font_[a-z0-9_]+\.bin)"', src)))


def loader_keys():
    """{file: set of top-level keys} as src/theme_style.cpp reads them."""
    src = open(os.path.join(ROOT, "src", "theme_style.cpp")).read()
    blocks = [(m.start(), m.group(1)) for m in
              re.finditer(r'read_style_json\(slug, "([a-z_]+\.json)", doc\)', src)]
    keys = {}
    for i, (pos, name) in enumerate(blocks):
        end = blocks[i + 1][0] if i + 1 < len(blocks) else len(src)
        body = src[pos:end]
        found = set(re.findall(r'doc\["([A-Za-z0-9_]+)"\]', body))
        found |= set(re.findall(r'\{\s*"([A-Za-z0-9_]+)",\s*[&a-z]', body))   # key tables
        keys.setdefault(name, set()).update(found)
    keys["theme.json"] = keys.get("theme.json", set()) | {"schema"}
    return keys


# ---- fonts ------------------------------------------------------------------------------

NORDIC = "æøåÆØÅ"


def font_codepoints(data):
    """The characters an lv_font_conv binary font carries, from its cmap table. None when the
    file is not one."""
    pos, cmap = 0, None
    while pos + 8 <= len(data):
        length, tag = struct.unpack_from("<I4s", data, pos)
        if length < 8:
            return None
        if tag == b"cmap":
            cmap = pos
            break
        pos += length
    if cmap is None:
        return None
    count, = struct.unpack_from("<I", data, cmap + 8)
    have = set()
    for i in range(count):
        off, start, rlen, _gid, entries, fmt = struct.unpack_from("<IIHHHB", data, cmap + 12 + i * 16)
        body = cmap + off
        if fmt == 2:                                   # format0_tiny: every code in the range
            have.update(range(start, start + rlen))
        elif fmt == 0:                                 # format0_full: an id per code, 0 = none
            have.update(start + k for k in range(rlen) if data[body + k])
        else:                                          # sparse (1) and sparse_tiny (3): deltas
            have.update(start + struct.unpack_from("<H", data, body + 2 * k)[0] for k in range(entries))
    return have


# ---- validate -----------------------------------------------------------------------------

class Report:
    def __init__(self):
        self.errors, self.warnings = [], []

    def error(self, msg):
        self.errors.append(msg)

    def warn(self, msg):
        self.warnings.append(msg)

    def show(self, folder):
        for m in self.errors:
            print("  error   " + m)
        for m in self.warnings:
            print("  warning " + m)
        verdict = "broken" if self.errors else "ok"
        print("%s: %s (%d error(s), %d warning(s))" % (folder, verdict, len(self.errors), len(self.warnings)))
        return 1 if self.errors else 0


def read_json(path, rep, required=False):
    name = os.path.basename(path)
    if not os.path.exists(path):
        if required:
            rep.error("%s is missing" % name)
        return None
    size = os.path.getsize(path)
    if size > STYLE_MAX:
        rep.error("%s is %d bytes; the Orb reads at most %d and ignores the whole file" % (name, size, STYLE_MAX))
        return None
    try:
        return json.load(open(path, encoding="utf-8"))
    except ValueError as e:
        rep.error("%s is not valid JSON: %s" % (name, e))
        return None


def validate(folder):
    rep = Report()
    slug = os.path.basename(os.path.normpath(folder))
    if len(slug) > SLUG_MAX:
        rep.error("the folder name '%s' is %d characters; the Orb keeps %d" % (slug, len(slug), SLUG_MAX))
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", slug):
        rep.warn("the folder name '%s' is best kept to lowercase letters, digits, - and _" % slug)
    if not os.path.exists(os.path.join(folder, "_installed")):
        rep.error("_installed is missing: the Orb lists only folders that have it")

    keys = loader_keys()
    theme = read_json(os.path.join(folder, "theme.json"), rep, required=True)
    if isinstance(theme, dict):
        schema = theme.get("schema")
        if schema is None:
            rep.warn('no "schema": read as an Orb Studio export (schema 1); add "schema": 1')
        elif schema != SCHEMA:
            rep.error('"schema": %r; this firmware reads schema %d' % (schema, SCHEMA))
        for k in sorted(set(theme) - keys["theme.json"]):
            rep.warn('theme.json: "%s" is not a key the Orb reads' % k)
        assets = theme.get("assets")
        if assets is None:
            rep.warn('no "assets" list: the Orb then trusts every file in the folder')
            assets = []
        known = {n for n, _, _ in BAKED} | set(CARD_ONLY) | set(font_files())
        for name in assets:
            if not isinstance(name, str):
                rep.error('"assets" holds something that is not a file name: %r' % (name,))
                continue
            if len(name) > 23:
                rep.error("%s: names over 23 characters cannot be baked or found" % name)
            if not os.path.exists(os.path.join(folder, name)):
                rep.error("%s is declared in theme.json but not in the folder" % name)
            if name not in known and not re.fullmatch(r"clock_plate_\d\d\.png", name):
                rep.warn("%s is declared but nothing in the firmware asks for it" % name)
        present = [f for f in os.listdir(folder) if f.endswith((".png", ".bin"))]
        for f in sorted(set(present) - set(assets)):
            if theme.get("assets") is not None:
                rep.warn("%s is in the folder but not declared, so the Orb ignores it" % f)

    for name in STYLE_FILES:
        doc = read_json(os.path.join(folder, name), rep)
        if isinstance(doc, dict):
            for k in sorted(set(doc) - keys.get(name, set())):
                rep.warn('%s: "%s" is not a top-level key the Orb reads' % (name, k))
        elif doc is not None:
            rep.error("%s should hold an object, not %s" % (name, type(doc).__name__))

    for f in sorted(os.listdir(folder)):
        if f.startswith("font_") and f.endswith(".bin"):
            have = font_codepoints(open(os.path.join(folder, f), "rb").read())
            if have is None:
                rep.error("%s is not an lv_font_conv binary font" % f)
            else:
                missing = "".join(c for c in NORDIC if ord(c) not in have)
                if missing and have & set(range(0x61, 0x7B)):   # only fonts that carry text
                    rep.warn("%s has no %s: Norwegian text in it draws with gaps" % (f, missing))
        if not f.endswith(".png"):
            continue
        try:
            w, h = orbpng.check(open(os.path.join(folder, f), "rb").read())
        except orbpng.NotOrbPng as e:
            rep.error("%s: %s" % (f, e))
            continue
        if f in FULL_SCREEN and (w, h) != (SCREEN, SCREEN):
            rep.error("%s is %dx%d; it fills the screen, so it must be %dx%d" % (f, w, h, SCREEN, SCREEN))
    return rep


# ---- estimate -----------------------------------------------------------------------------

def to_keys(rgba, alpha):
    """bake.cpp's key_at() for every pixel: RGB565 << 8 | alpha, every clear pixel one key."""
    keys = []
    for i in range(0, len(rgba), 4):
        r, g, b, a = rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]
        c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        if not alpha:
            keys.append((c << 8) | 0xFF)
        else:
            keys.append(((c << 8) | a) if a else 0)
    return keys


def baked_size(w, h, keys, alpha, indexable):
    """(bytes, description) as bake.cpp stores this picture."""
    raw = w * h * (3 if alpha else 2)
    if indexable:
        n = len(set(keys))
        if n <= 256:
            bpp = 1 if n <= 2 else 2 if n <= 4 else 4 if n <= 16 else 8
            return (4 << bpp) + ((w * bpp + 7) // 8) * h, "%d-bit indexed (%d colours)" % (bpp, n)
    return raw, "RGB565+A8" if alpha else "RGB565"


def estimate(folder):
    theme = json.load(open(os.path.join(folder, "theme.json"))) if os.path.exists(
        os.path.join(folder, "theme.json")) else {}
    declared = theme.get("assets")

    def wanted(name):
        return os.path.exists(os.path.join(folder, name)) and (declared is None or name in declared)

    seen, total, rows = {}, 0, []
    order = [(f, None, None) for f in font_files()]
    frames = sorted(f for f in os.listdir(folder) if re.fullmatch(r"clock_plate_\d\d\.png", f))
    order += [(f, False, False) for f in frames] + BAKED
    for name, alpha, indexable in order:
        if not wanted(name):
            continue
        path = os.path.join(folder, name)
        if alpha is None:                                   # a font: stored as it is
            blob = open(path, "rb").read()
            size, what, ident = len(blob), "font", hashlib.sha1(blob).hexdigest()
        else:
            try:
                w, h, rgba = orbpng.read_rgba(path)
            except orbpng.NotOrbPng as e:
                rows.append((name, 0, "not baked: %s" % e))
                continue
            keys = to_keys(rgba, alpha)
            size, what = baked_size(w, h, keys, alpha, indexable)
            what = "%dx%d %s" % (w, h, what)
            ident = hashlib.sha1(("%s|%d|%d|%s" % (what, w, h, keys)).encode()).hexdigest()
        if ident in seen:
            rows.append((name, 0, "shared with %s" % seen[ident]))
            continue
        seen[ident] = name
        padded = (size + SECTOR - 1) // SECTOR * SECTOR
        total += padded
        rows.append((name, padded, what))
    for name, size, what in rows:
        print("  %-24s %7.1f KB  %s" % (name, size / 1024, what))
    print("%s: %.2f MB baked, %.1f%% of the %.1f MB partition"
          % (folder, total / 1048576, 100.0 * total / PARTITION, PARTITION / 1048576))
    return 0


# ---- preview ------------------------------------------------------------------------------

def preview(folder):
    """Every app through the simulator's --themeshot, as BMPs beside the theme's name."""
    slug = os.path.basename(os.path.normpath(folder))
    program = os.path.join(ROOT, ".pio", "build", "native", "program")
    if not os.path.exists(program):
        print("building the simulator first (pio run -e native)")
        subprocess.check_call(["pio", "run", "-e", "native"], cwd=ROOT)
    dest = os.path.join(ROOT, "sim", "sdcard", "themes", slug)
    if os.path.abspath(dest) != os.path.abspath(folder):
        shutil.rmtree(dest, ignore_errors=True)
        shutil.copytree(folder, dest)
    with open("/tmp/orb_sim_theme_slug", "w") as f:   # theme_select.cpp NATIVE_SLUG_FILE
        f.write(slug + "\n")
    out = os.path.join(ROOT, "sim", "shots", slug)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.call([program, "--themeshot", out], cwd=ROOT)
    shots = sorted(f for f in os.listdir(os.path.dirname(out)) if f.startswith(slug + "-"))
    for f in shots:
        print("  " + os.path.join("sim", "shots", f))
    return 0 if shots else 1


def serve(folders, port=8000):
    """Offer theme folders over HTTP to an Orb running a development build with
    CONFIG_ORB_DEV_THEME_URL (main/dev_themes.cpp). /index.json lists each theme's files;
    /<slug>/<file> is the file. Plain HTTP on the local network: development only."""
    import http.server
    themes = {}
    for folder in folders:
        slug = os.path.basename(os.path.normpath(folder))
        rep = validate(folder)
        if rep.errors:
            print("note: %s has %d error(s); serving it anyway" % (slug, len(rep.errors)))
        themes[slug] = (folder, sorted(f for f in os.listdir(folder)
                                       if f != "_installed" and not f.startswith(".")
                                       and os.path.isfile(os.path.join(folder, f))))
    index = json.dumps({"themes": {s: files for s, (_, files) in themes.items()}}).encode()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            parts = self.path.strip("/").split("/")
            body = None
            if parts == ["index.json"]:
                body = index
            elif len(parts) == 2 and parts[0] in themes and parts[1] in themes[parts[0]][1]:
                body = open(os.path.join(themes[parts[0]][0], parts[1]), "rb").read()
            if body is None:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    addr = subprocess.run(["ipconfig", "getifaddr", "en0"], capture_output=True, text=True).stdout.strip()
    print("serving %s on port %d" % (", ".join(themes), port))
    print('in sdkconfig.dev: CONFIG_ORB_DEV_THEME_URL="http://%s:%d"' % (addr or "<this Mac's address>", port))
    try:
        http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
    except KeyboardInterrupt:
        print("stopped")
    return 0


def keys():
    for name, found in sorted(loader_keys().items()):
        print("%s (%d)" % (name, len(found)))
        print("  " + ", ".join(sorted(found)))
    print("fonts: " + ", ".join(font_files()))
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "keys":
        return keys()
    if len(argv) >= 3 and argv[1] == "serve":
        return serve(argv[2:])
    if len(argv) != 3 or argv[1] not in ("validate", "estimate", "preview"):
        print(__doc__)
        return 2
    folder = argv[2]
    if not os.path.isdir(folder):
        print("%s is not a folder" % folder)
        return 2
    if argv[1] == "validate":
        return validate(folder).show(folder)
    if argv[1] == "estimate":
        return estimate(folder)
    return preview(folder)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
