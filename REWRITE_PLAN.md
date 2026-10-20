# Orb OS rewrite — handoff from claude.ai chat (v3)

Context for Claude Code. This fork of `Ziplock78/orb-os` is being rebuilt in stages.
Read this file and the existing `CLAUDE.md` before changing anything. Where they
disagree, this file wins: the fork is leaving Orb Studio and the Arduino framework.

## Decisions (from the owner)

- **Pure ESP-IDF.** Drop the Arduino layer completely.
- **No Orb Studio.** Remove the Studio USB protocol, theme pull and Studio compatibility
  (`THEME_CAPS` parity) as constraints. Themes become this fork's own format.
- **No gateway / no Cloudflare Worker.** The device talks HTTPS directly to every
  service. The old `gateway/` worker files are obsolete; do not add them.
- **Norwegian news** from NRK RSS, with proper æøå.
- **New: weather from yr.no** (MET Norway API).
- **New, optional: internet radio** through the board's ES8311 codec and speaker.
- This is a single-owner device. A one-time settings reset on the ESP-IDF switch is
  acceptable if migration gets complicated, but keep WiFi credentials if it is cheap.

## What we found in the current code

- `src/main.cpp` is ~4,000 lines: boot, networking task, WiFi/NTP, web config page,
  location lookup and factory reset all live there.
- WiFi: WiFiManager `autoConnect` once at boot, a manual `WiFi.reconnect()` every 20 s
  after a drop, and a reboot to change networks.
- TLS: aircraft data already uses direct HTTPS (adsb.lol), but news, routes, ticker and
  theme pull go over plain HTTP to `INTEL_GATEWAY_HOST`
  (`buildtheorb.zionbrock.workers.dev`). TLS memory failures (`-32512`) are documented in
  `platformio.ini`; Arduino's prebuilt libs blocked tuning mbedTLS.
- Built-in fonts are ASCII only (`-r 0x20-0x7F`); æøå draw as boxes.
- Networking on core 0, LVGL on core 1. Keep that split.
- Touch driver exists and works (`src/touch_cst9217.cpp`) but is excluded; the Orb is
  knob-only by design.
- Theme art: baked once to raw pixels in a 9.6 MB `themeart` flash partition, drawn
  straight from memory-mapped flash (no PSRAM). Formats RGB565 (2 B/px) and RGB565+A8
  (3 B/px). A rich theme is ~3.8 MB. Art from deleted themes is never reclaimed; assets
  are stored per theme, so shared images are duplicated.
- OTA is compiled out: the partition table reclaimed the OTA slot for theme art
  (`ORB_OTA_ENABLED` off), although the README still describes OTA.
- Weather radar, stock ticker and camera exist but are compiled out (`APPS_LAUNCH_ONE`).
- Hardware (Waveshare ESP32-S3-Touch-AMOLED-1.75): ESP32-S3R8 (8 MB PSRAM, 16 MB flash),
  CO5300 AMOLED over QSPI, CST9217 touch, QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC,
  ES8311 audio + speaker, microSD. Pins in `src/config.h`.

## Must not break / must keep

- **Licence**: keep `LICENSE` and `NOTICE` (MIT, Quique Tortosa and Zion Brock).
- **Attribution on the About screen**: OpenStreetMap (ODbL), adsb.lol, and add
  MET Norway for weather (CC BY 4.0) and NRK as news source.
- **`nvs` partition offset/size** unchanged, so WiFi credentials survive flashing.
- **The desktop simulator**: UI code stays portable LVGL; platform code goes behind
  small interfaces so `native` still builds.
- **USB flashing** must keep working (`idf.py flash` and, if kept, the web flasher).

## Plan — one branch per stage, flash and test each before merging

### 1. `refactor/split-main` (Arduino, preparation)
Split `main.cpp` into `net/wifi`, `net/http`, `web_server`, `location`, `storage` and a
thin `main`. Move code only; no behaviour change. This makes the port mechanical.
(If already in progress, finish it before stage 2.)

### 2. `port/esp-idf` — new ESP-IDF project, module by module
- Project layout: `main/` + `components/` (`board`, `display`, `knob`, `net`, `ui`,
  `themes`, `apps/*`). Build with `idf.py` (or PlatformIO `framework = espidf`, pick one
  and document it). Pin the ESP-IDF version.
- Use Waveshare's ESP-IDF examples for this board as the reference for the CO5300
  panel, AXP2101, PCF85063, ES8311 and SD.
- Display: `esp_lcd` + `esp_lvgl_port`, staying on **LVGL v8** for this stage.
- Knob: use the **PCNT** peripheral for quadrature decoding instead of GPIO interrupts;
  keep the existing detent and rock-gesture logic from `knob.*` / `input_router.*`.
- sdkconfig: PSRAM octal, `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`,
  `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y`, certificate bundle
  (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`), mbedTLS allocations in PSRAM where safe.
  Commit `sdkconfig.defaults`, not the generated `sdkconfig`.
- Delete with the port: `orb_link.*` (Studio USB protocol), `theme_pull.*`,
  `ticker_client.*`, everything tied to `INTEL_GATEWAY_HOST`, WiFiManager, ArduinoOTA.
- Exit criterion: clock and flight tracker work on hardware and in the simulator.

### 3. `feature/wifi-state-machine`
Event-driven WiFi on `esp_event` (`WIFI_EVENT` / `IP_EVENT`): states
`IDLE → CONNECTING → CONNECTED → BACKOFF → PROVISIONING`; exponential backoff; several
saved networks with fallback; network switch without reboot; SoftAP + captive page
("The Orb Setup") as a state, not a separate boot path. Keep on-device setup with the knob.

### 4. `feature/net-client`
One HTTPS client task on core 0 (`esp_http_client` + cert bundle) with a request queue,
so only one TLS session is open at a time. Streaming parsers (JSON with a filter, XML
with ESP-IDF's `expat` component). Respect `Expires` / `Last-Modified` and send
`If-Modified-Since`. Per-app memory caps. Results to the UI via typed FreeRTOS queues.
Flight routes: call adsb.lol's route endpoint directly (verify the current API), or drop
route lookup if it is not available without the gateway.

### 5. `feature/themes-v2` — own theme format, no Studio
- A theme is a folder on the SD card: `/themes/<slug>/theme.json` + PNGs + optional
  font binaries. Versioned with a single `"schema": 1` field instead of `THEME_CAPS`.
- Document the schema in `docs/themes.md`; validate on load and show a clear error
  screen for a broken theme instead of silently drawing defaults.
- Baking (on device, from SD, as today) with:
  1. Reclaiming art for themes no longer on the card, at boot.
  2. Automatic format choice per image: alpha-only A8/A4 (recoloured) for single-colour
     hands, shadows and glows; 8-bit indexed palette for flat-colour plates; RGB565 /
     RGB565+A8 otherwise. All drawable straight from flash in LVGL v8.
  3. Content-addressed storage (hash of the PNG) so shared images are stored once.
  4. Later: skip the invisible corners of the round screen (~21% of a full-screen
     image) with per-row spans and a custom draw path.
- A host tool `tools/theme_tool.py`: validate a theme folder, show bake size estimate,
  and render previews through the simulator's `--themeshot`.
- Ship one or two default themes in the repo.
- Goal: shrink themes enough to restore an OTA slot (stage 10).

### 6. `feature/fonts-nordic`
Regenerate all built-in fonts with Norwegian characters and typographic punctuation:
`lv_font_conv ... -r 0x20-0x7F,0xA0-0xFF,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0x2026`
(covers æøåÆØÅ, é, «», –, —, quotes and …). Themes may bring their own fonts; the
validator in stage 5 warns if a theme font lacks æøå.

### 7. `feature/news-nrk`
- Fetch NRK RSS directly over HTTPS, parse with `expat` streaming, keep only what the
  screen needs (title, description, link hash as key). Keep full UTF-8.
- Feeds (verify each URL before use): `https://www.nrk.no/toppsaker.rss`,
  `/norge/toppsaker.rss`, `/urix/toppsaker.rss`, `/sport/toppsaker.rss`,
  `/viten/toppsaker.rss`, `/kultur/toppsaker.rss`, plus regional
  `/<region>/toppsaker.rss`. Feed list editable in settings / a JSON file on SD.
- Reuse the existing News screen (`intel_view.*`) and its briefing behaviour; rename
  `intel` → `news` while at it.
- Poll interval ≥ 5 minutes; use conditional requests.

### 8. `feature/weather-yr` — new app
- API: MET Norway Locationforecast 2.0,
  `https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=..&lon=..`
  (optionally `altitude=`). Add Nowcast later for "rain in the next 90 min".
- MET terms: send an identifying `User-Agent` with app name and contact (e.g.
  `orb-os/1.0 github.com/<owner>/orb-os`), round lat/lon to max 4 decimals, cache and
  honour `Expires`, use `If-Modified-Since`, never poll more often than needed.
  Credit "Data from MET Norway" (CC BY 4.0) on screen/About.
- Uses the Orb's existing location setting (centre point).
- Screen: now (temp, symbol, wind, precip), next hours, next days on a knob-scrolled
  wheel; themeable like the other apps.
- Icons: use the `symbol_code` values from the API; source icon artwork with a licence
  that permits use (check metno's weather icon repo licence) or draw our own set as
  alpha-only images.
- Do not revive the old weather radar; it was the memory problem.

### 9. `feature/radio` — optional, internet radio
- Audio path: ES8311 via `esp_codec_dev` + I2S; amp enable pin from the board example.
- Stream: HTTP(S) Icecast/Shoutcast MP3 or AAC with a PSRAM ring buffer; decoder from
  Espressif's `esp_audio_codec` (or libhelix MP3). Evaluate ESP-ADF only if it saves
  real work for this board — it is heavy.
- HLS-only stations (many broadcasters, possibly NRK) are much harder; start with
  stations that publish plain MP3/AAC stream URLs and verify each one.
- Station list in a JSON file on SD; knob turns = station/volume, press = play/stop.
- While playing: pause flight polling unless that screen is visible, and measure
  internal RAM / DMA headroom. The speaker is small; set expectations for quality.

### 10. `feature/settings-auth-ota`
- Versioned settings layer over NVS with migrations (namespace may stay `capsuleradar`).
- Web config page and reset endpoint behind a device password set during setup.
- Restore OTA: partition table with `ota_0`/`ota_1` + `otadata` (sized after stage 5
  shrinks theme art), `esp_https_ota`, signed images (secure boot v2 or app signature
  check), rollback on failed boot. Update the README.

### 11. `ci/build-and-screens`
GitHub Actions: build firmware with the pinned ESP-IDF, build the simulator, run its
capture modes and diff against reference images.

Deferred: LVGL v8 → v9 (native compressed images, better renderer); touch keyboard
(driver exists, design is knob-only; phone setup page covers WiFi entry).

## Git setup

```
git remote add upstream https://github.com/Ziplock78/orb-os.git
git fetch upstream
```
Upstream is now reference only; expect few clean merges after stage 2.

## Start here

Finish stage 1, then start stage 2 on a new branch with a minimal ESP-IDF project that
brings up the display and the knob before porting any app.
