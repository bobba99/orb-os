# PNGdec 1.1.6 by Larry Bank (BitBank Software), Apache License 2.0 (LICENSE beside this file).
#
# Copied unmodified from the copy PlatformIO installs for the Arduino and simulator builds
# (bitbank2/PNGdec 1.1.6; the device and simulator copies were byte-identical). It is not on
# Espressif's component registry, and the library has no ESP-IDF build file of its own. In
# the repository, its version is pinned by being here; update by replacing src/ whole.
idf_component_register(SRCS "src/PNGdec.cpp" "src/adler32.c" "src/crc32.c" "src/infback.c"
                            "src/inffast.c" "src/inflate.c" "src/inftrees.c" "src/zutil.c"
                       INCLUDE_DIRS "src")
# PNGdec's own "not an Arduino" switch, as the simulator build sets it (platformio.ini):
# without it PNGdec.h reaches for <Arduino.h>. Public, because it changes the header.
target_compile_definitions(${COMPONENT_LIB} PUBLIC __LINUX__)
# Not the ESP32-S3 SIMD colour conversion the Arduino build uses: s3_simd_rgb565.S needs
# Espressif's esp-dsp library, which Arduino-ESP32 bundles and this project does not carry.
# PNGdec falls back to its portable C path, which produces the same pixels more slowly; with
# theme art baked into flash a PNG is rarely decoded on the device at all.
# The same scanline buffer both other builds use: 512 px RGBA rows need it, 4096 cuts the
# right edge off.
target_compile_definitions(${COMPONENT_LIB} PUBLIC PNG_MAX_BUFFERED_PIXELS=8192)
