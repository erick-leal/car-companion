#include "ui.h"
#include "esp_log.h"

static const char *TAG = "ui";

esp_err_t ui_init(void)
{
    // TODO: inicializar el driver de pantalla (LovyanGFX/esp_lcd según el panel),
    // inicializar LVGL (lv_init), crear la tarea de tick + refresco de LVGL,
    // y mostrar una pantalla de "buscando adaptador OBD...".
    ESP_LOGW(TAG, "ui_init: no implementado todavía");
    return ESP_OK;
}

void ui_show_main_screen(void)        { ESP_LOGW(TAG, "TODO: ui_show_main_screen"); }
void ui_show_trip_stats_screen(void)  { ESP_LOGW(TAG, "TODO: ui_show_trip_stats_screen"); }
void ui_show_dtc_screen(void)         { ESP_LOGW(TAG, "TODO: ui_show_dtc_screen"); }
void ui_show_diagnostics_screen(void) { ESP_LOGW(TAG, "TODO: ui_show_diagnostics_screen"); }
void ui_show_maintenance_screen(void) { ESP_LOGW(TAG, "TODO: ui_show_maintenance_screen"); }
