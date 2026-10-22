# WHOLE_ARCHIVE: src/ (orb_ui) calls functions that live here, host_* and intel_fetch, and
# the linker only pulls an object out of an archive for a symbol something earlier asked for.
# An object nothing in main itself names (news.cpp) would otherwise be left out.
idf_component_register(SRCS "main.cpp" "host.cpp" "flight.cpp" "location.cpp" "routes.cpp" "theme_check.cpp" "dev_themes.cpp" "news.cpp" "bringup.cpp"
                       PRIV_REQUIRES orb_shared orb_ui board display orb_knob orb_sdcard
                                     orb_prefs orb_rtc orb_audio orb_wifi orb_net bblanchon__arduinojson espressif__expat esp_timer lvgl esp_system
                                     freertos
                       WHOLE_ARCHIVE)
