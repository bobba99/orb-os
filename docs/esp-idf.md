# Building the ESP-IDF firmware

Stage 2 of `REWRITE_PLAN.md` moves The Orb OS from Arduino to pure ESP-IDF. Until the port
is finished, two firmwares live in this repository side by side:

| | ESP-IDF (the port) | Arduino (the current firmware) |
|---|---|---|
| Source | `main/`, `components/` | `src/` |
| Build | `idf.py` | PlatformIO (`platformio.ini`) |
| Status | bring-up: display and knob | everything |

The desktop simulator still builds from `src/` with PlatformIO (`pio run -e native`) and is
not affected by anything here.

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
- The bring-up firmware never opens NVS, so it cannot change that network either.
- Going back is one Arduino flash: `pio run -e esp32-s3-amoled-175 -t upload`.

## Configuration

`sdkconfig.defaults` holds only what differs from ESP-IDF's defaults, with the reason beside
each line. The full `sdkconfig` is generated from it and not committed. After changing the
defaults, delete `sdkconfig` and build again.

## Names to avoid for components

`esp_lvgl_port` compiles its own encoder and button support whenever a component called
`knob` or `button` is in the build, expecting Espressif's libraries under those names. That
is why the knob component is `components/orb_knob` (its header is still `knob.h`). Do not
name a new component `button` either.
