#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * state_store — único punto de verdad del estado del vehículo en memoria.
 *
 * pid_engine ESCRIBE acá vía los setters (state_store_set_*). ui LEE de acá
 * (state_store_get) y/o se suscribe a cambios (state_store_subscribe).
 * Ninguna otra capa debería tocar estos structs directamente.
 *
 * Por qué setters y no un "partial struct" genérico: con setters explícitos
 * no hay ambigüedad de "qué campo cambió realmente" (un partial struct necesita
 * un bitmask de campos válidos aparte, que es fácil de desincronizar). Cuesta
 * un poco más escribir cada setter, pero es mucho más difícil de usar mal.
 */

#define STATE_STORE_MAX_SUBSCRIBERS 4

typedef struct {
    uint16_t rpm;
    uint8_t  speed_kmh;
    int16_t  coolant_temp_c;
    float    battery_voltage;
    int16_t  boost_pressure_kpa;   // MAP - barometrica (boost real); INT16_MIN si no soportado
    uint8_t  engine_load_pct;
    int16_t  intake_air_temp_c;
    bool     check_engine_on;
    uint8_t  throttle_pct;
    uint32_t fuel_rail_pressure_kpa; // common-rail diesel; 0 si no soportado/aun no leido
    float    fuel_rate_lph;
    int16_t  ambient_air_temp_c;
    uint8_t  barometric_pressure_kpa;
    bool     data_valid;           // false hasta la primera lectura real del OBD
    int64_t  last_update_us;       // esp_timer_get_time() del ultimo cambio, para detectar "datos viejos"
} vehicle_state_t;

typedef void (*state_change_cb_t)(const vehicle_state_t *state, void *ctx);

esp_err_t state_store_init(void);

/** Copia el estado actual (thread-safe) en out. */
esp_err_t state_store_get(vehicle_state_t *out);

/** Setters — cada uno actualiza un campo, marca data_valid=true y notifica suscriptores. */
esp_err_t state_store_set_rpm(uint16_t rpm);
esp_err_t state_store_set_speed(uint8_t speed_kmh);
esp_err_t state_store_set_coolant_temp(int16_t temp_c);
esp_err_t state_store_set_battery_voltage(float volts);
esp_err_t state_store_set_boost_pressure(int16_t kpa);
esp_err_t state_store_set_engine_load(uint8_t load_pct);
esp_err_t state_store_set_intake_air_temp(int16_t temp_c);
esp_err_t state_store_set_check_engine(bool on);
esp_err_t state_store_set_throttle(uint8_t pct);
esp_err_t state_store_set_fuel_rail_pressure(uint32_t kpa);
esp_err_t state_store_set_fuel_rate(float lph);
esp_err_t state_store_set_ambient_air_temp(int16_t temp_c);
esp_err_t state_store_set_barometric_pressure(uint8_t kpa);

/** Usado por ui para redibujar cuando cambian los datos. Maximo STATE_STORE_MAX_SUBSCRIBERS. */
esp_err_t state_store_subscribe(state_change_cb_t cb, void *ctx);
