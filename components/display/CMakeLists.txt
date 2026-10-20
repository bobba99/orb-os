idf_component_register(SRCS "display.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES board lvgl esp_lcd esp_driver_spi
                       PRIV_REQUIRES esp_lvgl_port esp_lcd_co5300)
