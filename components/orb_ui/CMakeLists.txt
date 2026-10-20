# The Orb OS — the UI, theme and app code, compiled straight from src/.
#
# Not copied. These are the files the simulator builds too (platformio.ini [env:native]), so
# every one of them already has a path that does not need Arduino, and compiling them in
# place means the simulator keeps testing the very code the device runs. Where a file's
# non-Arduino path is right for the simulator and wrong for the device, it gains an
# ESP-IDF branch of its own; see docs/esp-idf.md.
#
# The list is the clock's and the flight tracker's: stage 2's exit criterion. Hardware
# drivers are not here (display, knob, sdcard, audio and diag_log have ESP-IDF versions in
# their own components), and neither is anything that talks to the network yet.
set(ORB_SRC "${CMAKE_CURRENT_LIST_DIR}/../../src")
set(SHARED
    app_shell.cpp app_theme.cpp input_router.cpp knob_help.cpp update_ui.cpp
    clock_view.cpp clock_wind.cpp wind_notice.cpp
    radar_view.cpp ui.cpp radar_sprite.cpp coastline.cpp airports.cpp roads_sd.cpp
    route.cpp photo.cpp weather.cpp wx_radar.cpp cloud_image.cpp intel.cpp
    theme_style.cpp theme_select.cpp theme_sd.cpp theme_art.cpp theme_font.cpp theme_audio.cpp
    custom_sprite.cpp plate_sprite.cpp office_sprite.cpp menu_sprite.cpp menu_text.cpp
    splash_art.cpp splash_lines.cpp curved_text.cpp text_tokens.cpp
    custom_font1.c custom_font2.c custom_settings_font1.c
    custom_radar_font1.c custom_radar_font2.c custom_radar_font3.c custom_radar_font4.c
    custom_menu_font1.c custom_menu_font2.c custom_menu_font3.c
    font_inter_12.c font_inter_14.c font_inter_16.c font_inter_18.c font_inter_20.c)
list(TRANSFORM SHARED PREPEND "${ORB_SRC}/")
idf_component_register(SRCS ${SHARED}
                       REQUIRES orb_shared lvgl
                       PRIV_REQUIRES pngdec orb_prefs orb_sdcard orb_audio esp_timer esp_partition spi_flash)
# The shared files are held to the warning policy of the builds they came from, where a
# warning is reported and not fatal. They are compiled with every warning on and looked at;
# a real defect found that way is fixed in src/, and these flags only stop a style warning
# from blocking the port.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format -Wno-error=format-truncation
                                               -Wno-error=stringop-truncation -Wno-error=misleading-indentation)
