# The Arduino tree's headers, for this project's own components and nobody else's.
#
# During the port the UI, theme and app code is compiled straight from src/ rather than
# copied (see docs/esp-idf.md), so the simulator keeps testing the very code the device
# runs. src/ is not a global include path: it holds a config.h and a storage.h, and an
# ESP-IDF component must never find those in place of its own.
# esp_timer is public because src/orb_platform.h (millis, micros) includes it, and every
# component that can see src/ can include that.
idf_component_register(INCLUDE_DIRS "../../src"
                       REQUIRES esp_timer)
