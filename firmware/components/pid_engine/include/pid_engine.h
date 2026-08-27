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
    PID_MONITOR_STATUS        = 0x01, // bit7 de byte A = check engine (MIL) encendido
    PID_ENGINE_RPM            = 0x0C,
    PID_VEHICLE_SPEED         = 0x0D,
    PID_COOLANT_TEMP          = 0x05,
    PID_ENGINE_LOAD            = 0x04,
    PID_INTAKE_AIR_TEMP       = 0x0F,
    PID_INTAKE_MAP            = 0x0B, // Manifold Absolute Pressure (kPa) - se combina con PID_BAROMETRIC_PRESSURE para el boost real
    PID_THROTTLE_POS          = 0x11,
    PID_FUEL_RAIL_PRESSURE    = 0x23, // common-rail diesel, confirmado soportado en el Maxus T60 (ver firmware/README.md)
    PID_BAROMETRIC_PRESSURE   = 0x33,
    PID_AMBIENT_AIR_TEMP      = 0x46,
    PID_FUEL_RATE             = 0x5E,
} standard_pid_t;

esp_err_t pid_engine_init(void);

/**
 * Arranca una tarea FreeRTOS que hace polling round-robin de los PIDs
 * activos. Esta misma tarea tambien atiende, dentro del mismo loop, el
 * boton "CAPTURAR BUS" de la pantalla de Diagnostico (ver ui.c y el buzon
 * state_store_request_bus_capture_start/stop): mientras esa captura esta
 * activa, el polling normal de PIDs queda en pausa -- es ingenieria reversa
 * manual de PIDs propietarios (boost real, EGT), ver docs/pid-mapping.md.
 */
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

/**
 * Pide los codigos de falla activos (modo 03) y los escribe en state_store
 * cuando llegue la respuesta. A diferencia de los PIDs de arriba, esto NO es
 * parte del polling continuo — se dispara bajo pedido (ej. desde la
 * pantalla de Fallas), porque no tiene sentido pedirlo cada 150ms.
 */
esp_err_t pid_engine_read_dtc_codes(void);

/**
 * Borra los codigos de falla activos y apaga el testigo CHECK (modo 04).
 * Igual que la lectura, se dispara bajo pedido desde ui via el buzon de
 * state_store. Si el ECU realmente tenia una falla activa (no solo
 * historica), el codigo puede volver a aparecer en la proxima lectura —
 * borrar no arregla la causa, solo el registro.
 */
esp_err_t pid_engine_clear_dtc_codes(void);
