# Named orb_knob, not knob: esp_lvgl_port compiles its own encoder support whenever a
# component called "knob" (or "button") is in the build, expecting Espressif's iot_knob
# library under that name, and the build fails on ours. The header is still knob.h.
idf_component_register(SRCS "knob.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board esp_driver_pcnt esp_driver_gpio esp_timer)
