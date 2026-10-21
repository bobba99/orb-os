# Named orb_wifi: ESP-IDF has an esp_wifi component, and this is the Orb's use of it.
# portal.cpp is "The Orb Setup": the access point's captive DNS and setup page.
idf_component_register(SRCS "orb_wifi.cpp" "portal.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer lwip esp_http_server
                                     orb_prefs)
