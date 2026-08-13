#include "storage.h"
#include "esp_log.h"

static const char *TAG = "storage";

esp_err_t storage_init(void)
{
    // TODO: nvs_flash_init() para config; montar SD (SPI) o usar partición SPIFFS/FAT
    // para el historial de viajes si no hay slot de SD en el hardware final.
    ESP_LOGW(TAG, "storage_init: no implementado todavía");
    return ESP_OK;
}

esp_err_t storage_save_trip_record(const void *record, size_t len)
{
    ESP_LOGW(TAG, "storage_save_trip_record: no implementado todavía");
    return ESP_OK;
}

esp_err_t storage_get_pending_sync_count(int *count)
{
    *count = 0;
    return ESP_OK;
}
