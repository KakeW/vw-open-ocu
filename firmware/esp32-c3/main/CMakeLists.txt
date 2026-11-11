idf_component_register(SRCS gateway.c INCLUDE_DIRS "."
    REQUIRES driver esp_event esp_netif esp_timer esp_wifi mqtt nvs_flash mbedtls)
