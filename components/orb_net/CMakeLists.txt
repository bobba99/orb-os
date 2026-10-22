idf_component_register(SRCS "orb_net.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES bblanchon__arduinojson
                       PRIV_REQUIRES orb_shared orb_wifi esp_http_client esp_timer mbedtls)
