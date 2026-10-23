# Building the ESP-IDF firmware

Stage 2 of `REWRITE_PLAN.md` moves The Orb OS from Arduino to pure ESP-IDF. Until the port
is finished, two firmwares live in this repository side by side:

| | ESP-IDF (the port) | Arduino (the current firmware) |
|---|---|---|
| Source | `main/`, `components/` | `src/` |
| Build | `idf.py` | PlatformIO (`platformio.ini`) |
| Status | stage 2 done: clock and flight tracker, on WiFi with the live feed | everything |

The desktop simulator still builds from `src/` with PlatformIO (`pio run -e native`) and is
not affected by anything here.

## How the shared code reaches ESP-IDF

The UI, theme and app code is not copied. `components/orb_ui` compiles it straight from
`src/`, the same files the simulator builds, so the simulator keeps testing the very code
the device runs. Three things make that work:

- **`src/orb_platform.h`** gives pure ESP-IDF the few Arduino calls the shared code makes
  (`Serial.printf`/`println`, `millis`, `micros`, `delay`, `ESP.getFreePsram`/`restart`),
  as thin wrappers over ESP-IDF, and says where the card is mounted (`ORB_SD_ROOT`). On
  Arduino it is just `<Arduino.h>`. The names stay Arduino's for now so the shared files
  compile unchanged on all three builds; when the Arduino build retires they become neutral
  names in one search-and-replace.
- **Three branches where there were two.** A shared file that chose between Arduino and the
  simulator with `#ifdef ARDUINO` gains `#elif defined(ESP_PLATFORM)` where the device needs
  something the simulator does not: NVS instead of a file in `/tmp`, the card's real root.
  The Arduino and simulator branches are never edited, and every change is checked against
  both: the Arduino build's sizes and warnings, the simulator build, `tools/audit.py`.
- **Drivers implement the existing contracts.** `components/display` implements
  `src/display.h`, `orb_knob` implements `src/knob.h`, and so on for `sdcard.h`, `audio.h`
  and `rtc_pcf85063.h`. The shared code does not know which firmware it is in.

`components/orb_shared` puts `src/` on the include path of this project's components only.
`src/` holds a `config.h` and a `storage.h`, and ESP-IDF components must never find those in
place of their own.

### What runs, and what does not yet

| Ported | Not yet |
|---|---|
| Display (CO5300, LVGL 8, byte-swapped flush) | Arbitrary rotation, the TE wait, the staged flush |
| Knob on PCNT, rock gesture, long-press reboot | |
| SD card (SPI, 20/10/4 MHz negotiation, never formats) | |
| Settings in NVS, same keys as Arduino, never erases | Some settings: what the clock, the scope and Settings use is read |
| RTC seeds the clock at boot | NTP, writing NTP time back to the RTC |
| Clock, Flight Tracker, Headlines (NRK and VG), Weather (MET Norway) and Settings apps, theme, splash | The web server |
| WiFi: several saved networks, fallback, backoff, "The Orb Setup", joining with the knob; NTP; the aircraft feed over verified HTTPS | Forgetting a network from the screen |
| One HTTPS client task (`orb_net`): the feed, flight routes, Settings > Location (search, recents, IP lookup) | Settings > Reset erases nothing (stage 10) |
| Audio: ES8311 and the speaker (beeps, alerts, the hour's chime, theme sounds) | IMU, battery, idle dimming; internet radio (stage 9) |
| Theme art and fonts baked into the themeart partition at boot (stage 5 store: shared blobs, indexed formats, reclaiming) | Skipping the round screen's invisible corners |

### Boot to the bring-up page

For the first flash of a new display or knob driver, before the apps are trusted on it, build
with `sdkconfig.bringup` layered on, in its own folder so the app build's configuration is
left alone:

```bash
idf.py -B build-bringup -D SDKCONFIG=build-bringup/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bringup" -p /dev/cu.usbmodem21101 flash
```

That sets `CONFIG_ORB_BRINGUP_PAGE=y`. The Orb then boots to a
test page that exercises only the panel and the knob (`main/bringup.cpp`) and opens neither
the card nor NVS.

## Stage 2: done (2026-10-21)

The exit criterion, the clock and the flight tracker working on hardware and in the
simulator, was met on an Orb on 2026-10-21:

- **Bring-up page:** the panel draws round and true (the ring reaches the edge evenly, red,
  green and blue in order, text upright) and the knob counts, presses and rocks.
- **Clock:** the owner's Arduino-chosen theme, read from NVS; plates, overlays and 13 of 28
  font slots read straight from the baked `themeart` partition; time from the RTC, then NTP.
- **Flight Tracker:** WiFi, NTP, and the aircraft feed over HTTPS with the certificate
  verified (`api.adsb.lol -> 200`, aircraft drawn on the scope), polled by `net_task.cpp`'s
  rules. Tested on a phone hotspot through `sdkconfig.dev`, because the Orb was away from
  its saved network.
- **Simulator:** builds from the same `src/` and renders both apps (`--shot`).
- **The Arduino build** kept building throughout, its sizes checked after every change to
  `src/`, as did `tools/audit.py`'s output.

Against the plan's list for the stage: `idf.py` with ESP-IDF pinned; Waveshare's board
package as the reference; `esp_lcd` + `esp_lvgl_port` on LVGL 8; the knob on PCNT with the
detent and rock logic kept; the sdkconfig items; `sdkconfig.defaults` committed. Two
departures, both deliberate: the knob component is `orb_knob` (see "Names to avoid"), and
`ui`, `themes` and `apps` are one `orb_ui` component compiling them from `src/` (see "How
the shared code reaches ESP-IDF").

**Not deleted yet, on purpose:** the plan says to delete `orb_link.*`, `theme_pull.*`,
`ticker_client.*`, the `INTEL_GATEWAY_HOST` clients, WiFiManager and ArduinoOTA "with the
port". The ESP-IDF firmware compiles none of them. Their source stays while the Arduino
firmware does, because it still uses all of them and the simulator builds two
(`ticker_client`, `intel_client`). They go when the Arduino build is retired.

**The stages after this one, in the plan's order:** the setup portal, joining and saving
networks (3, done); one HTTPS client task (4, done); themes v2 and baking on ESP-IDF (5, done);
Nordic fonts (6, done); NRK and VG news, which brought Headlines back (7, done); yr.no weather
(8, done); radio and the ES8311 (9); Settings, auth and OTA (10); CI (11). Smaller gaps from this stage, none of them in the plan's list,
are in "What runs, and what does not yet" above. Two are worth measuring rather than
leaving: the QSPI clock (40 MHz here, 80 on Arduino) and PNGdec's C path.

## Stage 3: WiFi, done (2026-10-22)

`components/orb_wifi` is an event-driven state machine on `esp_event`: IDLE, CONNECTING,
CONNECTED, BACKOFF, PROVISIONING. One manager task on core 0 makes every decision; its header
says how a round works and the two promises it keeps (the driver's stored network is read once
and never written; a network is saved only once it has produced an address).

- **Several saved networks** in NVS (`wifiNets`, up to 8), seeded from the Arduino firmware's
  stored network, tried strongest first; backoff from 1 s doubling to 30 s; no reboots.
- **"The Orb Setup"**, an open access point beside the station (APSTA), with captive DNS and
  a page at 192.168.4.1, opens after `CONFIG_ORB_WIFI_SETUP_AFTER_S` (300 s) offline or when no
  network is saved, and closes 60 s after the Orb is back online.
- **Settings** is ported whole (`src/settings_view.cpp` compiled as it is, its host functions
  in `main/host.cpp`). Its WiFi pages drive `orb_wifi::tryNetwork()`; a join needs no restart.
  Pages whose work belongs to a later stage say so in the log and change nothing.

Seen on the Orb: the list seeded, fallback, the backoff ladder, rejoining after the link went,
the setup network opening and listing networks, a wrong password refused with the saved
networks left as they were, and a join with the knob. Merged without seeing a join from the
setup page, or the setup network closing after it.

**A router that announces a channel switch and stays put** makes any ESP32 drop every few
seconds: the Orb follows the announcement (`sta rx csa, newchan=8, old=6`), hears no beacons
on the new channel, and leaves with reason 200. ESP-IDF has no setting to ignore the
announcement. The cure is on the router: give its 2.4 GHz radio a fixed channel.

## Stage 4: one HTTPS client (2026-10-22)

`components/orb_net` replaces `orb_http`. One task on core 0 takes requests from a queue, so only
one TLS session is ever open; `fetch()` blocks its caller until the answer is in. Its header has
the rules (no `fetch()` from a handler, no display lock in a handler).

- **Streaming:** a request's handler reads the body as it arrives (`orb_net::Body` is an
  ArduinoJson reader), so the aircraft feed is parsed off the socket with its filter instead of
  being buffered whole. XML (stage 7) goes through expat the same way.
- **Freshness:** a `cached` request is not sent while `max-age` or `Expires` (against the
  server's own `Date`) says the last answer holds, and is revalidated with `If-None-Match` /
  `If-Modified-Since` after; validators live in RAM, 24 URLs.
- **Memory caps:** each app's documents come from its own PSRAM allocator with a cap.
- **Results to the UI** go through each app's own hand-off (the flight snapshot,
  `route_store()`); the city search is synchronous because Settings' interface is.

On it so far:

- **The aircraft feed**, as before.
- **Location** (`main/location.cpp`): Open-Meteo city search; the IP lookup from ipapi.co,
  falling back to ipwho.is (ip-api.com, which Arduino used, refuses HTTPS without a paid key);
  recents under the Arduino key. A place is applied while the Orb runs and the Flight Tracker
  comes up; nothing restarts. The timezone keeps its daylight-saving rule for ~40 common zones
  and falls back to Arduino's fixed offset elsewhere. A never-located Orb asks once per boot.
- **Flight routes** (`main/routes.cpp`) without the gateway: adsb.lol's static file per
  callsign (`vrs-standing-data.adsb.lol/routes/XX/CALLSIGN.json`; its `/api/0/route` redirects
  there and `routeset` answers empty), showing the first and last IATA code. Ten minutes'
  freshness and an ETag, so the cache does real work; the answers are kept in RAM.

Seen on the Orb (phone hotspot): the feed, an IP lookup with its timezone, the city search and
picking a place, and routes from the network and from the cache. Measured: the client task's
stack has at least 5.3 KB of its 10 KB left after a handshake and a parse; the largest free
internal block fell to 11-16 KB with Settings built, and handshakes still succeeded.

IP geolocation is only as good as the network's address: over a phone it found the carrier's
city, not the Orb's. Settings > Location > Search is the cure.

## Stage 5: themes (2026-10-22)

The theme format is this fork's own, written down in `docs/themes.md`: schema 1 is the folder
layout Orb Studio exported, with `"schema": 1` in `theme.json` in place of `THEME_CAPS`.

- **Theme art store** (`components/orb_ui/themeart/`), layout version 9, ESP-IDF only (Arduino
  keeps `src/theme_art.cpp`): pictures stored once however many screens or themes use them,
  freed room reused, art of themes gone from the card reclaimed at boot. The baker stores a
  picture of 256 colours or fewer as an LVGL indexed image and checks every pixel first; it
  also bakes the splash and radar glass, rings, shadows and winding screen, which Arduino read
  off the card each time. Cold War: 4.1 MB before, 2.36 MB now, with more of it baked.
- **A theme that does not load whole** is named on a system screen at boot
  (`main/theme_check.cpp`).
- **Tools:** `tools/theme_tool.py` (validate, estimate, preview, keys, serve) and
  `tools/make_default_theme.py`, which writes the shipped default `themes/orb-plain/`.
- **Development:** `CONFIG_ORB_DEV_THEME_URL` copies themes from a computer over WiFi.

Seen on the Orb: the Cold War rebake and every screen of it; Plain (bake 668 KB against the
tool's 0.65 MB estimate); a copy of Plain stored with no extra bytes; the error screen for a
schema-2 theme. Not done: running `theme_tool.py validate` on a real Orb Studio theme, which
needs the card in a computer. Left for later: skipping the round screen's invisible corners.

## Stage 6: Norwegian letters in the built-in fonts (2026-10-22)

`tools/gen_fonts.py` regenerates every built-in font with Latin-1 and typographic punctuation
(`0x20-0x7F, 0xA0-0xFF, – — ‘ ’ “ ” • …`):

- **Montserrat**, every size `include/lv_conf.h` turns on, from LVGL's own source and with its
  own recorded options (icons included), into `src/fonts/` under LVGL's names. `lv_conf.h`
  switches LVGL's ASCII-only copies off and declares these; no call site changed. The same
  options without the new range reproduce LVGL's files byte for byte.
- **Inter** (the splash's lines) from `tools/fonts/InterVariable.ttf`, Inter 4.1 under the OFL,
  the file that reproduces the old fonts' metrics exactly.
- **Text from the network** is fitted to its font (`src/orb_text_fit.h`) instead of folded to
  ASCII, and ALL CAPS reaches Latin-1 (`src/orb_text_case.h`).
- `tools/audit.py` fails if a built-in font loses the range; `theme_tool.py validate` warns
  about a theme font without æøå.

Cost: about 100 KB more glyph pixels and the kerning that goes with them; the ESP-IDF image is
4.96 MB of a 6.4 MB partition. The compiled design's own faces (`custom_menu_font1`,
`custom_radar_font1/2`) are left: they were made from fonts not in the repository and carry
only the characters that design drew. Themes bring their own.

## Stage 7: news from NRK and VG (2026-10-22)

Headlines is back, reading NRK's and VG's RSS feeds directly (`main/news.cpp`; the format and
`/news.json` are in `docs/news.md`). The screen is Arduino's (`src/intel_view.cpp`, unchanged
apart from an ESP-IDF WiFi check); `main/news.cpp` implements the two calls it makes,
`intel_fetch()` and `intel_brief_fetch()`, which went to the Orb Studio gateway on Arduino.

- One feed per publisher, chosen by the theme's topic; Settings > News (ESP-IDF only) picks NRK,
  VG or both. Parsed with expat as it arrives, 20 stories a feed at most, merged newest first.
- Summaries come from each story's `<description>`, kept with the list, so a press needs no
  second request.
- Conditional requests through `orb_net`, and a round at most every five minutes.
- About gains "News: NRK, VG · Places: Open-Meteo".
- `main/CMakeLists.txt` links `main` WHOLE_ARCHIVE: `src/` calls functions defined here, and an
  object nothing in `main` itself names was being left out of the link.

Seen on the Orb: both feeds (NRK 20 stories from 22 KB of a 109 KB feed, VG 10), æøå on screen,
a story's summary, Settings > News cycling the sources, the third credit line inside the circle.

Not done, on purpose: the plan's rename of `intel` to `news`. Those files are shared with the
Arduino firmware, which still reads the gateway through them; the rename goes with Arduino.

## Stage 8: Weather from MET Norway (2026-10-22)

A new Weather app (`docs/weather.md`): now, then a wheel of the next 12 hours and 7 days, from
MET Norway's Locationforecast for the Orb's location (`main/yr.cpp` fetches through `orb_net`;
`src/yr_view.cpp` draws, in the simulator too with `--yrshot`). Twelve plain icons drawn by
`tools/make_weather_icons.py`, in the theme's colour. Wind in m/s.

- MET's terms: the User-Agent on ESP-IDF names this fork (`ORB_USER_AGENT`, every service gets
  it), coordinates to four decimals, `Expires`/`If-Modified-Since` through the cache, at most one
  request in ten minutes. Credited under the forecast and on About.
- A Weather slot after Headlines (`app_shell::Slot`, ESP-IDF only). Shown by default; an Orb
  Studio theme's `weather` flag and name meant the old radar app and are ignored here.

Also fixed on the way (its own commit): the hand-drawn text paths looked glyphs up a byte at a
time, so after stage 6 "ø" drew as "Ã¸" in themed text. They decode UTF-8 now (`src/orb_utf8.h`).

Seen on the Orb: a live forecast (35 KB, 0.6 s), now, the hours and the days on the wheel, a
press back to now, the MET credit on About.

## Stage 9, first part: audio (2026-10-23)

The plan's stage 9 is internet radio and optional. Its first step, the speaker itself, is done
on its own: `components/orb_audio` is no longer a stub.

- `audio.cpp` ports src/audio.cpp: the same ES8311 register sequence (Espressif's
  esp_codec_dev for this board), the same cues, interruption by a newer request, volume curves,
  and the hour's chime that nothing cuts short. Underneath: `orb_i2c`, the current `i2s_std`
  driver (16 kHz, 16-bit stereo, MCLK 256 fs), the amplifier pin (46) on only while sounding.
- The codec's analog supply, AXP2101 ALDO1 at 3.3 V, is switched on by `audio_begin()`, which
  writes only that rail's enable bit and voltage field. On the Orb it was already on.
- Wired: the saved volume, levels and mute (`host.cpp`); the hour's chime while the clock is on
  screen (`main.cpp`, as src/main.cpp); new-aircraft, emergency and proximity alerts on each new
  snapshot (`flight.cpp`, src/app_hooks.cpp's rules); the clock's ticks and the winding sounds,
  which already called the real interface.
- Cost: the recorded Westminster chime is 448 KB of flash. The ESP-IDF image is 5.64 MB of the
  6.4 MB app partition (10% free), which stage 10's OTA layout has to plan for.

Heard on the Orb: the volume beep and the Westminster chime preview, from the speaker.

Not done: streaming radio (decoder, stations, ring buffer), the rest of the plan's stage 9.

## The toolchain: `idf.py`, ESP-IDF v5.5.6

`idf.py` rather than PlatformIO's `framework = espidf`, for two reasons:

- PlatformIO takes its source folder from one project-wide `src_dir` setting. An ESP-IDF
  environment in the existing `platformio.ini` would have moved the Arduino firmware and the
  simulator off `src/` mid-port. With `idf.py` the two trees cannot touch.
- Waveshare's examples for this board, Espressif's component registry and the plan's own
  flashing instructions all assume `idf.py`.

**Pinned to ESP-IDF v5.5.6**, the newest 5.5 release. 5.5 is what Waveshare builds this
board's examples against. The root `CMakeLists.txt` stops the build on any other version,
so a binary is never built against an ESP-IDF that was not tested. To move to a new version,
change `ORB_IDF_VERSION` there and the `idf:` line in `components/display/idf_component.yml`
together, and flash and test on an Orb.

Components come from Espressif's registry. The exact versions are recorded in
`dependencies.lock`, which is committed; `managed_components/` is downloaded from it.

| Component | Version | Why |
|---|---|---|
| `lvgl/lvgl` | 8.4.0 | the version the Arduino firmware runs; v9 is deferred |
| `espressif/esp_lvgl_port` | 2.9.0 | LVGL task, locking and flush (the plan's choice) |
| `espressif/esp_lcd_co5300` | 2.2.0 | the panel driver Waveshare's own board package uses |

Waveshare's complete board package (`waveshare/esp32_s3_touch_amoled_1_75`) is used as a
reference, not a dependency. Its 2.x and 3.x releases drive LVGL through
`esp_lvgl_adapter` rather than `esp_lvgl_port`, and they bring a USB stack and an IO-expander
driver this design does not use. The pins in `components/board/include/board.h` agree with it.

## One-time setup (macOS)

```bash
mkdir -p ~/esp && cd ~/esp
git clone --depth 1 --branch v5.5.6 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git esp-idf-v5.5.6
cd esp-idf-v5.5.6 && ./install.sh esp32s3
python3 tools/idf_tools.py install cmake ninja
```

The last line is needed on macOS: `install.sh` expects CMake and Ninja on the `PATH` and
does not fetch them. `idf_tools.py` puts its own copies in `~/.espressif`. It is run with
`python3` explicitly because its first line asks for `python`, which a pyenv setup may not
provide.

## Build, flash, watch

In each new terminal, load the environment first:

```bash
. ~/esp/esp-idf-v5.5.6/export.sh
```

Then, from the repository root:

```bash
idf.py build
idf.py -p /dev/cu.usbmodem21101 flash monitor
```

Ctrl+] leaves the monitor. The port name is the one PlatformIO auto-detects. Orb Studio's
**My Orb** tab or a running `pio device monitor` holds the port, so close them first.

## What flashing this does to an Orb

- It writes the bootloader, the partition table and the app. The partition table is the
  Arduino build's own `partitions_16MB_themeart.csv`, read directly rather than copied, so
  `nvs` keeps its offset and size and the WiFi network the Arduino firmware saved survives.
- The firmware's own settings go only through `components/orb_prefs`, which uses the namespace
  `capsuleradar` and nothing else and never erases the partition, even when ESP-IDF reports
  it needs to. It reads and writes the same keys as the Arduino firmware, with the same
  encodings, so either firmware can be flashed over the other. The WiFi driver reads the
  saved network from its own namespace and is switched to RAM-only storage straight after
  (`components/orb_wifi`), so this firmware can never overwrite it. ESP-IDF's radio driver
  does keep its calibration in an NVS namespace of its own (`phy`), as Arduino-ESP32 does.
- The SD card is never formatted, even if it will not mount.
- Going back is one Arduino flash: `pio run -e esp32-s3-amoled-175 -t upload`.

## Working away from the Orb's network

The Orb joins the network it has saved. To test somewhere else, put that network in a
git-ignored `sdkconfig.dev` and build it in its own folder:

```bash
printf 'CONFIG_ORB_DEV_WIFI_SSID="name"\nCONFIG_ORB_DEV_WIFI_PASS="password"\n' > sdkconfig.dev
idf.py -B build-dev -D SDKCONFIG=build-dev/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.dev" -p /dev/cu.usbmodem21101 flash
```

That network is joined in RAM only; the saved one is untouched and comes back with the next
ordinary build. The radio is 2.4 GHz only, so a 5 GHz-only network can never be joined, and the
boot log lists every network the scan heard when the wanted one is not among them.
`CONFIG_ORB_WIFI_COUNTRY` (default `01`, channels 1 to 11) can open channels 12 and 13 where
they are allowed, for example `NO`.

## Configuration

`sdkconfig.defaults` holds only what differs from ESP-IDF's defaults, with the reason beside
each line. The full `sdkconfig` is generated from it and not committed. After changing the
defaults, delete `sdkconfig` and build again.

## Names to avoid for components

`esp_lvgl_port` compiles its own encoder and button support whenever a component called
`knob` or `button` is in the build, expecting Espressif's libraries under those names. That
is why the knob component is `components/orb_knob` (its header is still `knob.h`). Do not
name a new component `button` either.
