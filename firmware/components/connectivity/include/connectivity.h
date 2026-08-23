#pragma once
#include "esp_err.h"

/**
 * connectivity — WiFi + sync HTTP con el backend.
 * Todo no bloqueante: si no hay WiFi, el dispositivo debe seguir funcionando
 * como gauge OBD standalone (esto es un requisito de producto, no opcional).
 *
 * Modelo de uso real: el M5 vive en el auto, sin WiFi la mayor parte del
 * tiempo (manejando). connectivity_init() arranca WiFi STA + una tarea de
 * fondo que reintenta sincronizar cada 30s SOLO si hay conexion (si no,
 * el chequeo es gratis, no hace ningun llamado de red) — cuando el auto
 * vuelve a estar cerca del WiFi de casa, sincroniza solo, sin que el
 * usuario tenga que hacer nada.
 *
 * Autenticacion: el backend hoy solo tiene JWT de usuario (login por
 * email/contraseña, 30 dias), no token de dispositivo separado — ver
 * docs/api-contract.md. connectivity hace login antes de cada sync para
 * sacar un token fresco, usando credenciales de
 * connectivity_secrets.h (NO se sube a git, ver connectivity_secrets.example.h).
 *
 * OTA (connectivity_check_ota) queda afuera de esta primera version —
 * ver TODO en connectivity.c.
 */

esp_err_t connectivity_init(void);

/** Chequea si hay firmware nuevo publicado y lo aplica (esp_https_ota). */
esp_err_t connectivity_check_ota(void);

/**
 * Intenta sincronizar los viajes pendientes con el backend ahora mismo.
 * No bloquea si no hay WiFi/hora valida: devuelve ESP_ERR_INVALID_STATE de
 * inmediato en ese caso. Normalmente no hace falta llamarla a mano — la
 * tarea de fondo de connectivity_init() ya la llama sola cuando corresponde.
 */
esp_err_t connectivity_sync_trip_history(void);
