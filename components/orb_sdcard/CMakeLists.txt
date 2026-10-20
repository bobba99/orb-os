# Implements src/sdcard.h (found through orb_shared) on SDSPI + FAT. Named orb_sdcard because
# ESP-IDF has its own "sdmmc" and "fatfs" components and this is neither.
idf_component_register(SRCS "sdcard.cpp"
                       REQUIRES orb_shared
                       PRIV_REQUIRES board fatfs sdmmc esp_driver_sdspi esp_driver_spi)
