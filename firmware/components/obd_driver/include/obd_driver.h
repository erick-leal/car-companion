#pragma once
#include "esp_err.h"
#include <stdint.h>

/**
 * obd_driver — habla con el adaptador OBD (Vgate vLinker MC+) por BLE.
 *
 * Esta capa NO interpreta el significado de los datos (eso es pid_engine).
 * Solo sabe: conectar, enviar comandos AT/PID crudos, y devolver la respuesta cruda.
 */

typedef void (*obd_response_cb_t)(const uint8_t *data, size_t len, void *ctx);

/** Inicia el escaneo BLE y conexión con el adaptador OBD configurado. No bloqueante. */
esp_err_t obd_driver_init(void);

/** Envía un comando PID crudo (ej. "010C" para RPM) y registra el callback de respuesta. */
esp_err_t obd_driver_send_command(const char *pid_hex, obd_response_cb_t cb, void *ctx);

/** true si hay conexión BLE activa con el adaptador. */
bool obd_driver_is_connected(void);
