#pragma once
#include "esp_err.h"
#include <stdint.h>

/**
 * state_store — único punto de verdad del estado del vehículo en memoria.
 *
 * pid_engine ESCRIBE acá. ui LEE de acá (y se suscribe a cambios).
 * Ninguna otra capa debería tocar estos structs directamente.
 */

typedef struct {
    uint16_t rpm;
    uint8_t  speed_kmh;
    int16_t  coolant_temp_c;
    float    battery_voltage;
    int16_t  boost_pressure_kpa;   // -1 si no soportado por el vehículo
    uint8_t  engine_load_pct;
    int16_t  intake_air_temp_c;
    bool     check_engine_on;
    bool     data_valid;           // false si aún no hay lectura real del OBD
} vehicle_state_t;

typedef void (*state_change_cb_t)(const vehicle_state_t *state, void *ctx);

esp_err_t state_store_init(void);

/** Copia el estado actual (thread-safe) en out. */
esp_err_t state_store_get(vehicle_state_t *out);

/** Usado por pid_engine para actualizar un campo y notificar a los suscriptores. */
esp_err_t state_store_update(const vehicle_state_t *partial);

/** Usado por ui para redibujar cuando cambian los datos. */
esp_err_t state_store_subscribe(state_change_cb_t cb, void *ctx);
