#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * pid_engine — traduce respuestas OBD crudas (de obd_driver) en valores con
 * significado y los escribe en state_store.
 *
 * Los PIDs estándar (SAE J1979) se parsean en pid_parser.c (formulas fijas,
 * iguales para cualquier vehiculo). Los PIDs propietarios del Maxus T60
 * (turbo, EGT si existen) van aparte en pid_table_maxus.c — todavia sin
 * descubrir, ver docs/pid-mapping.md.
 */

typedef enum {
    PID_ENGINE_RPM            = 0x0C,
    PID_VEHICLE_SPEED         = 0x0D,
    PID_COOLANT_TEMP          = 0x05,
    PID_ENGINE_LOAD            = 0x04,
    PID_INTAKE_AIR_TEMP       = 0x0F,
    PID_INTAKE_MAP            = 0x0B, // Manifold Absolute Pressure (kPa) - proxy de boost si el motor es turbo
} standard_pid_t;

esp_err_t pid_engine_init(void);

/** Arranca una tarea FreeRTOS que hace polling round-robin de los PIDs activos. */
esp_err_t pid_engine_start_polling(void);

/**
 * Parsea la respuesta cruda del ELM327 a un PID de modo 01 (formato:
 * "41 0C 1A F8" en texto, ya sin espacios/prompt) y escribe el valor
 * resultante en state_store. Expuesto en el header para poder testearlo
 * unitariamente sin BLE real (ver firmware/test/test_pid_parser.c).
 */
esp_err_t pid_engine_parse_mode01_response(uint8_t pid, const uint8_t *data_bytes, size_t len);

/** Parsea la respuesta de texto de "ATRV" (ej. "12.6V") para el voltaje de bateria. */
esp_err_t pid_engine_parse_atrv_response(const char *response_text);
