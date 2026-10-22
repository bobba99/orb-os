idf_component_register(SRCS "main.cpp" "host.cpp" "flight.cpp" "location.cpp" "routes.cpp" "theme_check.cpp" "dev_themes.cpp" "bringup.cpp"
                       PRIV_REQUIRES orb_shared orb_ui board display orb_knob orb_sdcard
                                     orb_prefs orb_rtc orb_audio orb_wifi orb_net bblanchon__arduinojson esp_timer lvgl esp_system
                                     freertos)
