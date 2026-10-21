# Implements src/display.h (found through orb_shared) for ESP-IDF, plus display_idf.h.
idf_component_register(SRCS "display.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES orb_shared lvgl
                       PRIV_REQUIRES board esp_lcd esp_driver_spi esp_timer
                                     esp_lvgl_port esp_lcd_co5300)
