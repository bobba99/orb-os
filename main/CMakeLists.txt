idf_component_register(SRCS "main.cpp" "host.cpp" "flight.cpp" "bringup.cpp"
                       PRIV_REQUIRES orb_shared orb_ui board display orb_knob orb_sdcard
                                     orb_prefs orb_rtc orb_audio orb_wifi lvgl esp_system
                                     freertos)
