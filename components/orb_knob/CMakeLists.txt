# Implements src/knob.h (found through orb_shared) on the PCNT peripheral.
#
# Named orb_knob, not knob: esp_lvgl_port compiles its own encoder support whenever a
# component called "knob" (or "button") is in the build, expecting Espressif's iot_knob
# library under that name, and the build fails on ours.
idf_component_register(SRCS "knob.cpp"
                       REQUIRES orb_shared
                       PRIV_REQUIRES board esp_driver_pcnt esp_driver_gpio esp_timer)
