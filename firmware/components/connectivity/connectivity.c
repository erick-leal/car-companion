#include "connectivity.h"
#include "esp_log.h"

static const char *TAG = "connectivity";

esp_err_t connectivity_init(void)
{
    // TODO: WiFi provisioning (esp_wifi + guardar credenciales en NVS,
    // o BLE provisioning si no queremos pantalla de config compleja).
    ESP_LOGW(TAG, "connectivity_init: no implementado todavía");
    return ESP_OK;
}

esp_err_t connectivity_check_ota(void)
{
    // TODO: GET a backend/api/firmware/latest, comparar versión, esp_https_ota si aplica.
    ESP_LOGW(TAG, "connectivity_check_ota: no implementado todavía");
    return ESP_OK;
}

esp_err_t connectivity_sync_trip_history(void)
{
    // TODO: leer de storage lo pendiente de sync, POST al backend, marcar como sincronizado.
    ESP_LOGW(TAG, "connectivity_sync_trip_history: no implementado todavía");
    return ESP_OK;
}
