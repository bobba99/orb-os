# Named orb_wifi: ESP-IDF has an esp_wifi component, and this is the Orb's use of it.
idf_component_register(SRCS "orb_wifi.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer lwip orb_prefs)
