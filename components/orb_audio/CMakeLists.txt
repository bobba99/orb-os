# Implements src/audio.h (found through orb_shared): the ES8311 codec and the speaker. Named
# orb_audio: ESP-IDF projects often carry an "audio" component of their own from the ADF, and
# this is not that.
idf_component_register(SRCS "audio.cpp"
                       REQUIRES orb_shared
                       PRIV_REQUIRES orb_i2c esp_driver_i2s esp_driver_gpio esp_driver_i2c)
