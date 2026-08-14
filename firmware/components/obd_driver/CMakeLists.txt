idf_component_register(
    SRCS "obd_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash esp_timer
)
