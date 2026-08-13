#pragma once
#include "esp_err.h"

/**
 * connectivity — WiFi provisioning, OTA y sync HTTP con el backend.
 * Todo no bloqueante: si no hay WiFi, el dispositivo debe seguir funcionando
 * como gauge OBD standalone (esto es un requisito de producto, no opcional).
 */

esp_err_t connectivity_init(void);

/** Chequea si hay firmware nuevo publicado y lo aplica (esp_https_ota). */
esp_err_t connectivity_check_ota(void);

/** Sube el historial local de viajes pendiente de sync al backend. */
esp_err_t connectivity_sync_trip_history(void);
