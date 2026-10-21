idf_component_register(SRCS "orb_i2c.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES board)
