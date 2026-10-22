# Themes (schema 1)

A theme is how The Orb looks: its dials, plates, glass, hands, colours, type and layout. It is a
folder on the SD card, and this page is its format. Stage 5 of `REWRITE_PLAN.md` made it this
fork's own: no Orb Studio, no `THEME_CAPS` handshake, one `"schema"` number.

Schema 1 is the layout Orb Studio exported, written down. Every theme already on a card is a
schema-1 theme; new ones say so with `"schema": 1`.

## The folder

```
/themes/<slug>/
  _installed              empty; the Orb lists only folders that carry it
  theme.json              required: what the theme is and what it ships
  clock_style.json        optional, one per screen (see below)
  ...
  clock_plate.png         pictures, 8-bit RGBA PNG
  ...
  font_clock1.bin         optional typefaces (lv_font_conv binaries)
```

- **The slug** is the folder's name: up to 31 characters, best kept to `a-z`, `0-9`, `-` and `_`.
  It is what the Orb remembers as the active theme.
- **`_installed`** marks a finished theme. A folder without it is never listed, which is what
  keeps a half-copied theme out of Settings > Theme.

Copy the folder to `/themes/` on the card and choose it in Settings > Theme. The first boot in it
bakes its pictures into flash (below), which takes some seconds once.

## theme.json

```json
{
  "schema": 1,
  "name": "Plain",
  "author": "The Orb OS",
  "apps": { "clock": true, "flight": true, "headlines": true },
  "names": { "flight": "Radar" },
  "assets": ["clock_plate.png", "clock_overlay.png", "..."],
  "assetsHash": 123456789
}
```

| Key | Meaning |
|---|---|
| `schema` | The format's version, `1`. A firmware refuses a schema it does not read and says so on screen. Absent means an Orb Studio export, which is read as schema 1. |
| `name`, `author` | Shown in Settings > Theme and on About. |
| `apps` | Which apps the knob reaches: `clock`, `flight`, `weather`, `surveillance`, `headlines`, `ticker`. Settings is always there. Absent keeps the firmware's own roster. |
| `names` | Display names for those apps (and `settings`). Cosmetic only. |
| `assets` | Every picture and font the theme ships. The Orb reads and bakes only these, so a stale file left on the card is ignored. Absent means "trust every file in the folder". |
| `assetsHash` | Optional. Any number that changes when a picture's contents change; it is what makes the Orb bake again after an image is replaced under the same name. Without it, only a change to the `assets` list does. |

## The screens' files

Each is optional; a file that is absent, or a key that is absent, keeps the firmware's compiled
value for it. Each file must stay under 8 KB, and must be valid JSON: otherwise the Orb ignores
the whole file and tells you on screen.

| File | Screen |
|---|---|
| `clock_style.json` | The clock: hands (`hands.hour/minute/second/static1/static2`, `hands.order`, `hands.shadow`), text banners `text1`/`text2`, sweep, winding |
| `radar_style.json` | The Flight Tracker: sweep, aircraft symbols, selection, centre, rings, map, labels, keep-out zones |
| `menu_style.json` | The app switcher's three lines: `current`, `prev`, `next` |
| `settings_style.json` | Settings' wheel: colours, glow, highlight, geometry |
| `splash_style.json` | The boot card and About page |
| `intel_style.json` | Headlines |
| `ticker_style.json`, `weather_style.json` | Apps this firmware does not ship yet |

The full list of keys per file is the loader's: `python3 tools/theme_tool.py keys` prints it,
straight out of `src/theme_style.cpp`, and the field-by-field meaning is in the structs of
`src/theme_style.h`. Colours are numbers (`0xRRGGBB`, written in JSON as decimal), opacities
0..255 unless the header says otherwise.

A hand is a picture of the hand pointing at twelve. `pivotX`/`pivotY` is the pixel in that
picture it turns about; `centerX`/`centerY` is where on the screen that pixel sits (233, 233 is
the middle).

Text formats (`fmt`) are `strftime` formats: `"%a %d"` is "Thu 22".

## Pictures

**8-bit RGBA PNG, and nothing else.** The Orb keeps a pixel only from that kind of file; an RGB,
palette or greyscale PNG draws black, which is why `theme_tool.py validate` treats one as an
error. Full-screen pictures are 466 x 466.

| Picture | Where | Notes |
|---|---|---|
| `clock_plate.png` | The dial | Opaque. `clock_plate_01.png` upward: frames of a moving dial |
| `clock_overlay.png` | Glass over the hands | The hub usually lives here |
| `clock_hand_hour/minute/second.png`, `clock_static1/2.png` | Hands and fixed layers | Pointing at twelve |
| `clock_shadow_hour/minute/second.png` | Hand shadows | |
| `menu_plate.png`, `menu_overlay.png` | App switcher | |
| `settings_plate.png`, `settings_overlay.png` | Settings | |
| `radar_plate.png`, `radar_overlay.png`, `radar_rings.png` | Flight Tracker | |
| `radar_sweep.png`, `radar_blip.png`, `radar_static1/2.png`, `radar_card.png` | Flight Tracker layers | |
| `splash.png`, `splash_overlay.png` | Boot card and About | |
| `wind_bg.png`, `wind_crank.png`, `wind_crank_shadow.png` | The winding screen | |
| `intel_plate.png`, `intel_overlay.png` | Headlines | |

## Fonts

Optional. A theme may carry its own faces as `lv_font_conv` binaries under the slot names
`theme_tool.py keys` lists (`font_clock1.bin`, `font_menu_current.bin`, ...). A slot the theme does
not fill uses the firmware's compiled face. This repository's `.gitignore` ignores `*.bin`, so a
theme kept here with fonts needs them added with `git add -f`.

## Baking

Reading a PNG off the card and unpacking it costs about 0.4 s and 424-636 KB of memory each time
a screen shows it. So on the first boot after a theme is chosen or changed, the Orb unpacks each
declared picture once and stores the pixels in a 9.6 MB flash partition (`themeart`), where they
are drawn with no reading and no unpacking. The screen says "Preparing theme" meanwhile.

The baker (`components/orb_ui/themeart/bake.cpp`) chooses how to store each picture:

- **Indexed** when it has 256 colours or fewer, counting a colour at each level of transparency
  once: a palette and 1, 2, 4 or 8 bits a pixel. A plate of 85 colours halves; a glass of one
  colour at one transparency drops from 3 bytes a pixel to 1 bit. Only pictures whose readers
  understand indexed pixels are eligible (plates, overlays, rings, the clock's glass); the clock
  dial, its frames, hands and shadows are copied and turned pixel by pixel and stay as they are.
  Every indexed picture is unpacked again and compared with the original before it is kept.
- **Once.** A picture whose pixels are already stored, by this theme or another, is not stored
  again: one glass used on three screens takes the room of one.
- **Reclaimed.** At boot, the art of a theme no longer on the card is dropped and its room
  reused. Not when the card is missing, and not when the list of themes cannot be trusted.

`python3 tools/theme_tool.py estimate <folder>` prints what the bake will store, picture by
picture, and the total against the partition.

## When something is wrong

At boot the Orb checks the active theme (`main/theme_check.cpp`): `theme.json` is there, parses,
and has a schema it reads; every screen's file that is there parses and is under 8 KB; every
declared picture is on the card or already baked. The first problem found is shown on a plain
screen, "This theme did not load completely", with the reason; a push carries on with whatever
did load.

`python3 tools/theme_tool.py validate <folder>` checks the same and more, on a computer: the
PNGs' kind and size, names too long for the Orb, files that are declared but missing or present
but undeclared, and keys the Orb does not read (usually a typing mistake).

## Tools

```
python3 tools/theme_tool.py validate themes/orb-plain   # is it whole?
python3 tools/theme_tool.py estimate themes/orb-plain   # what will the bake store?
python3 tools/theme_tool.py preview  themes/orb-plain   # every app, through the simulator
python3 tools/theme_tool.py keys                        # every key each file may hold
python3 tools/make_default_theme.py                     # writes themes/orb-plain/
```

`preview` copies the folder into `sim/sdcard/themes/`, makes it the simulator's active theme and
runs `--themeshot`, which writes one BMP per app to `sim/shots/`. It opens the simulator's window
for a few seconds.

## Sending a theme over WiFi (development)

For an Orb whose card the computer cannot read. On the computer:

```
python3 tools/theme_tool.py serve themes/orb-plain [more folders]
```

It prints the line to put in the git-ignored `sdkconfig.dev`
(`CONFIG_ORB_DEV_THEME_URL="http://<this computer>:8000"`). Build and flash the development build
(`docs/esp-idf.md`); once WiFi is up it copies every theme offered into `/themes/` on the card
(`main/dev_themes.cpp`), and they can be chosen in Settings > Theme. The Orb and the computer
must be on the same network, and the server has to run in an ordinary terminal (it listens for
connections). It only writes into the folders it was sent, never deletes, and marks a theme
installed only when every file arrived. Plain HTTP: development only, never in
`sdkconfig.defaults`.

## The default theme

`themes/orb-plain/` is written by `tools/make_default_theme.py` from nothing but numbers: a dark
dial with minute and hour marks, three slim hands and a hub, plain plates elsewhere. It belongs
to this repository outright, under its licence. It carries no fonts, so its type is the
firmware's compiled faces. Baked, it takes 0.65 MB.
