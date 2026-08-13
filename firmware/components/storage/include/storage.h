#pragma once
#include "esp_err.h"

/**
 * storage — persistencia local (NVS para config, SD/flash para historial de viajes)
 * antes de que connectivity los sincronice con el backend.
 */

esp_err_t storage_init(void);

esp_err_t storage_save_trip_record(const void *record, size_t len);
esp_err_t storage_get_pending_sync_count(int *count);
