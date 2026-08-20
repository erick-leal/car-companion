idf_component_register(
    SRCS "ui.c" "core2_power.c" "core2_display.c" "core2_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES state_store driver esp_lcd
)
