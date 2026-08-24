#pragma once
#include "esp_err.h"

/**
 * connectivity — WiFi + sync HTTP con el backend.
 * Todo no bloqueante: si no hay WiFi, el dispositivo debe seguir funcionando
 * como gauge OBD standalone (esto es un requisito de producto, no opcional).
 *
 * Modelo de uso real: el M5 vive en el auto, sin WiFi la mayor parte del
 * tiempo (manejando). El radio WiFi queda APAGADO por default (elegido con
 * el usuario el 24 ago, tras medir ~5% de bateria gastado en <10min con el
 * M5 prendido sin conectar a nada — dejar el radio siempre activo
 * esperando reconectar sale caro en bateria). connectivity_init() solo
 * inicializa el stack de WiFi, sin prender el radio; una tarea de fondo lo
 * prende por una ventana acotada (WIFI_CONNECT_WINDOW_MS) cada
 * SYNC_CHECK_INTERVAL_MS (~12min) SI hay algo pendiente para sincronizar, o
 * al tocar el boton de sync en la pantalla de Viaje — y lo apaga de nuevo
 * al terminar el intento, haya conectado o no. Ver attempt_sync_window()
 * en connectivity.c.
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
