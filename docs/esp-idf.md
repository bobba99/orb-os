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
| Settings in NVS, same keys as Arduino, never erases | Most settings: only theme, app skin, wind, tz, brightness, place name are read |
| RTC seeds the clock at boot | NTP, writing NTP time back to the RTC |
| Clock and Flight Tracker apps, theme, splash | Headlines, Settings, the web server |
| WiFi on the stored network, reconnecting with backoff; NTP; the aircraft feed over verified HTTPS | The setup portal, joining or saving a new network (stage 3) |
| | Audio (a stub reporting no codec), IMU, battery, idle dimming |
| Baked theme art and fonts, read from the themeart partition | Baking: a theme installed or changed is not re-baked yet |

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

**Left for later stages, in the order the plan has them:** the setup portal, joining and
saving networks (3); one HTTPS client task with typed queues, which also removes the
shared-state globals (4); themes v2 and baking on ESP-IDF (5); Nordic fonts (6); NRK news,
which brings Headlines back (7); yr.no weather (8); radio and the ES8311 (9); Settings,
auth and OTA (10); CI (11). Smaller gaps from this stage, none of them in the plan's list,
are in "What runs, and what does not yet" above. Two are worth measuring rather than
leaving: the QSPI clock (40 MHz here, 80 on Arduino) and PNGdec's C path.

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
