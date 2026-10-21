# Implements src/rtc_pcf85063.h (found through orb_shared) on the shared orb_i2c bus.
idf_component_register(SRCS "rtc_pcf85063.cpp"
                       REQUIRES orb_shared
                       PRIV_REQUIRES orb_i2c)
