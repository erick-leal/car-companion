idf_component_register(
    SRCS "connectivity.c"
    INCLUDE_DIRS "include"
    REQUIRES storage state_store esp_wifi esp_netif esp_event esp_http_client json mbedtls nvs_flash esp_timer esp_https_ota app_update esp_app_format
)
