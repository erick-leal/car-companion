#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * obd_driver — habla con el adaptador OBD (Vgate vLinker MC+) por BLE.
 *
 * Esta capa NO interpreta el significado de los datos (eso es pid_engine).
 * Solo sabe: escanear/conectar por BLE, enviar comandos AT/PID como texto
 * ASCII (protocolo ELM327), y entregar la respuesta cruda (también ASCII)
 * a un callback.
 *
 * IMPORTANTE: los UUIDs de servicio/característica BLE están en
 * obd_driver_config.h como placeholders — hay que confirmarlos con el
 * adaptador real (ver instrucciones en ese archivo) antes de que esto conecte.
 */

typedef void (*obd_response_cb_t)(const uint8_t *data, size_t len, void *ctx);

/** Inicializa NimBLE y arranca el escaneo/conexión con el adaptador configurado.
 *  No bloqueante: la conexión real sucede en background vía el GAP event handler. */
esp_err_t obd_driver_init(void);

/**
 * Envía un comando de texto (ej. "010C" para RPM, o "ATRV" para voltaje) y
 * registra el callback que recibe la respuesta cruda cuando llegue.
 *
 * Solo soporta UN comando en vuelo a la vez (ver nota en obd_driver.c) —
 * es responsabilidad del llamador (pid_engine) esperar la respuesta o el
 * timeout antes de mandar el siguiente. Esto es intencional para v1: el
 * protocolo ELM327 es inherentemente request/response secuencial, no hay
 * beneficio real en pipelinear comandos.
 */
esp_err_t obd_driver_send_command(const char *command, obd_response_cb_t cb, void *ctx);

/** true si hay conexión BLE activa y el adaptador ya respondió al init (ATZ/ATE0). */
bool obd_driver_is_connected(void);
