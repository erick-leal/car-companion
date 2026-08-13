#include "pid_engine.h"
#include "esp_log.h"

static const char *TAG = "pid_engine";

esp_err_t pid_engine_init(void)
{
    // TODO: cargar tabla de PIDs estándar + tabla de PIDs propietarios del Maxus T60
    ESP_LOGW(TAG, "pid_engine_init: no implementado todavía");
    return ESP_OK;
}

esp_err_t pid_engine_start_polling(void)
{
    // TODO: crear tarea FreeRTOS que pida PIDs activos a obd_driver cada N ms
    // y escriba los valores parseados en state_store.
    ESP_LOGW(TAG, "pid_engine_start_polling: no implementado todavía");
    return ESP_OK;
}
