# The Orb OS — the UI, theme and app code, compiled straight from src/.
#
# Not copied. These are the files the simulator builds too (platformio.ini [env:native]), so
# every one of them already has a path that does not need Arduino, and compiling them in
# place means the simulator keeps testing the very code the device runs. Where a file's
# non-Arduino path is right for the simulator and wrong for the device, it gains an
# ESP-IDF branch of its own; see docs/esp-idf.md.
#
# The list is the clock's, the flight tracker's (stage 2) and Settings' (stage 3). Hardware
# drivers are not here (display, knob, sdcard, audio and diag_log have ESP-IDF versions in
# their own components). adsb_client fetches through orb_net on ESP-IDF.
set(ORB_SRC "${CMAKE_CURRENT_LIST_DIR}/../../src")
set(SHARED
    app_shell.cpp app_theme.cpp input_router.cpp knob_help.cpp update_ui.cpp
    clock_view.cpp clock_wind.cpp wind_notice.cpp adsb_client.cpp
    radar_view.cpp ui.cpp radar_sprite.cpp coastline.cpp airports.cpp roads_sd.cpp
    route.cpp photo.cpp weather.cpp wx_radar.cpp cloud_image.cpp intel.cpp
    theme_style.cpp theme_select.cpp theme_sd.cpp  theme_font.cpp theme_audio.cpp
    custom_sprite.cpp plate_sprite.cpp office_sprite.cpp menu_sprite.cpp menu_text.cpp
    splash_art.cpp splash_lines.cpp curved_text.cpp text_tokens.cpp
    settings_view.cpp settings_sprite.cpp settings_text.cpp chime_library.cpp
    custom_font1.c custom_font2.c custom_settings_font1.c
    custom_radar_font1.c custom_radar_font2.c custom_radar_font3.c custom_radar_font4.c
    custom_menu_font1.c custom_menu_font2.c custom_menu_font3.c
    font_inter_12.c font_inter_14.c font_inter_16.c font_inter_18.c font_inter_20.c)
list(TRANSFORM SHARED PREPEND "${ORB_SRC}/")
# Montserrat with Norwegian letters (tools/gen_fonts.py), in place of LVGL's: see lv_conf.h.
file(GLOB ORB_FONTS "${ORB_SRC}/fonts/*.c")
list(APPEND SHARED ${ORB_FONTS})
# The theme art store and its baker for ESP-IDF (stage 5). src/theme_art.cpp is the Arduino
# firmware's store, with a different on-flash layout, and is not compiled here.
list(APPEND SHARED "${CMAKE_CURRENT_LIST_DIR}/themeart/store.cpp" "${CMAKE_CURRENT_LIST_DIR}/themeart/bake.cpp")
idf_component_register(SRCS ${SHARED}
                       REQUIRES orb_shared lvgl
                       PRIV_REQUIRES pngdec orb_prefs orb_sdcard orb_audio orb_net orb_wifi esp_timer
                                    esp_partition spi_flash)
# The shared files are held to the warning policy of the builds they came from, where a
# warning is reported and not fatal. They are compiled with every warning on and looked at;
# a real defect found that way is fixed in src/, and these flags only stop a style warning
# from blocking the port.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format -Wno-error=format-truncation
                                               -Wno-error=stringop-truncation -Wno-error=misleading-indentation)
