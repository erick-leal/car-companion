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
#define STATE_STORE_MAX_DTC 8 // suficiente para el uso real: si un auto tiene mas de 8 fallas activas, ya hay un problema mas grande que la UI

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
    char     dtc_codes[STATE_STORE_MAX_DTC][6]; // ej. "P0301", vacio ("") = slot sin usar
    uint8_t  dtc_count;
    bool     dtc_read_in_progress;  // para que ui muestre "leyendo..." en vez de "sin fallas" mientras espera
    bool     dtc_clear_in_progress; // idem para "borrando..." mientras se manda el modo 04
    bool     bus_capture_active;   // true mientras pid_engine esta en modo "escuchar todo el bus" (ATMA), ver mas abajo
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
/** codes[i] son strings de hasta 5 chars + '\0' (ej "P0301"), count <= STATE_STORE_MAX_DTC. */
esp_err_t state_store_set_dtc_codes(const char codes[][6], uint8_t count);
esp_err_t state_store_set_dtc_read_in_progress(bool in_progress);
esp_err_t state_store_set_dtc_clear_in_progress(bool in_progress);

/**
 * "Buzon de pedidos" de un solo item: ui llama a state_store_request_dtc_read()
 * (ej. al tocar un boton) y pid_engine llama a
 * state_store_consume_dtc_read_request() en su loop para ver si hay algo
 * pendiente. Es la unica via para que ui dispare una accion "hacia arriba"
 * sin importar pid_engine directamente (ver regla de dependencias en
 * firmware/README.md — ui solo conoce state_store).
 */
esp_err_t state_store_request_dtc_read(void);
/** true (una sola vez) si habia un pedido pendiente; lo limpia al leerlo. */
bool state_store_consume_dtc_read_request(void);

/** Mismo patron de buzon que arriba, para el boton "BORRAR CODIGOS" (modo 04). */
esp_err_t state_store_request_dtc_clear(void);
bool state_store_consume_dtc_clear_request(void);

/**
 * Buzon para el boton "CAPTURAR BUS" de la pantalla de Diagnostico
 * (ingenieria reversa manual de PIDs propietarios, ver docs/pid-mapping.md):
 * arranca/para el modo "escuchar todo el bus" (ATMA) en vez del polling
 * normal de PIDs. `pid_engine` fija bus_capture_active en vehicle_state_t
 * para que `ui` muestre si esta activo.
 */
esp_err_t state_store_request_bus_capture_start(void);
bool state_store_consume_bus_capture_start_request(void);
esp_err_t state_store_request_bus_capture_stop(void);
bool state_store_consume_bus_capture_stop_request(void);
esp_err_t state_store_set_bus_capture_active(bool active);

/**
 * Mismo patron de buzon, para el boton de sincronizar a mano en la pantalla
 * de Viaje — `connectivity` ya sincroniza solo cada 30s en segundo plano,
 * esto solo adelanta el proximo intento sin esperar ese ciclo.
 */
esp_err_t state_store_request_sync(void);
bool state_store_consume_sync_request(void);

/**
 * Resultado del ultimo intento de sync (pedido real del 24 ago: el boton
 * de sync manual no daba ningun feedback, "deberia decir algo o no
 * tenemos nada"). `connectivity` lo fija al principio y al final de cada
 * `attempt_sync_window()` (tanto si fue disparado por el boton como por el
 * ciclo automatico); `ui` lo consulta con un timer mientras esta en la
 * pantalla de Viaje. No es "estado del vehiculo" (no notifica
 * suscriptores), mismo criterio que el reloj de pared de arriba.
 */
typedef enum {
    SYNC_STATUS_IDLE = 0,        // ningun intento todavia en este arranque
    SYNC_STATUS_IN_PROGRESS,     // WiFi prendido, conectando/sincronizando ahora
    SYNC_STATUS_NOTHING_PENDING, // no habia ningun viaje pendiente -- el radio ni se prendio (ojo: un viaje EN CURSO no cuenta como pendiente todavia, recien se guarda al terminar)
    SYNC_STATUS_OK,              // habia viaje(s) pendiente(s) y se mandaron con exito al backend
    SYNC_STATUS_NO_WIFI,         // ultimo intento no encontro el WiFi de casa a tiempo
    SYNC_STATUS_ERROR,           // WiFi conecto pero el envio al backend fallo (ver log serial para el detalle)
} sync_status_t;

esp_err_t state_store_set_sync_status(sync_status_t status);
sync_status_t state_store_get_sync_status(void);

/**
 * Reloj de pared aproximado del dispositivo — NO es "estado del vehiculo"
 * (no toca data_valid ni dispara notify_subscribers, ui no necesita
 * redibujar por esto), asi que vive aparte de vehicle_state_t, mismo
 * criterio que los buzones de arriba.
 *
 * `connectivity` la fija una vez que SNTP sincroniza (ver
 * on_sntp_time_synced en connectivity.c): offset_s = hora real UNIX -
 * esp_timer_get_time()/1e6, constante durante todo el arranque una vez
 * fijada. `storage` la lee al cerrar un viaje para guardar una hora real
 * aproximada junto al viaje (ver trip_record_t.recorded_at_epoch_s en
 * storage.h) — sin esto, `connectivity` solo podia reconstruir la hora de
 * un viaje asumiendo que paso en el arranque ACTUAL, lo cual es incorrecto
 * para viajes de un arranque anterior que recien se sincronizan despues
 * (bug real encontrado el 23-24 ago: dos viajes de dias distintos quedaron
 * con la misma fecha en el backend).
 */
esp_err_t state_store_set_wall_clock_offset(int64_t offset_s);
/** true si ya se fijo al menos una vez en este arranque (deja *offset_s sin tocar si no). */
bool state_store_get_wall_clock_offset(int64_t *offset_s);

/**
 * VIN (numero de chasis) del vehiculo actualmente conectado -- permite
 * saber en que auto se esta capturando datos si el mismo M5 (o el mismo
 * adaptador) se usa en mas de un vehiculo (pedido real del 25 ago).
 * `pid_engine` lo lee una sola vez por conexion (modo 09 PID 02) y lo fija
 * aca; `connectivity` lo manda con cada viaje al sincronizar. Mismo
 * criterio que el reloj de pared: no es "estado del vehiculo" en el
 * sentido de vehicle_state_t, no notifica suscriptores.
 */
esp_err_t state_store_set_vin(const char *vin);
/** true si hay un VIN conocido en este arranque (deja out_vin sin tocar si no). out_vin debe tener 18 bytes. */
bool state_store_get_vin(char out_vin[18]);

/**
 * Marca data_valid=false (el adaptador OBD se desconecto — reason real de
 * manejo: el Vgate se resetea con la caida de tension al arrancar el motor,
 * o queda fuera de rango momentaneamente). Deja el resto de los campos
 * (ultima lectura conocida) intactos, para que si `ui` quiere mostrarlos
 * como "ultimo dato visto" en vez de borrarlos de golpe, pueda — hoy no lo
 * hace, pero no hay razon para destruir el dato.
 *
 * IMPORTANTE: antes de esto no existia forma de volver data_valid a false
 * una vez que se ponia true la primera vez — el dashboard se quedaba
 * mostrando "OBD OK" con datos viejos para siempre despues de una
 * desconexion real. Encontrado revisando errores tipicos de manejo (22 ago).
 * `storage` tambien depende de este flag para cerrar un viaje cuando el OBD
 * se desconecta (ver storage.c) — sin este setter esa rama nunca se
 * ejecutaba, y todos los viajes terminaban solo por el timeout de 15min.
 */
esp_err_t state_store_set_disconnected(void);

/** Usado por ui para redibujar cuando cambian los datos. Maximo STATE_STORE_MAX_SUBSCRIBERS. */
esp_err_t state_store_subscribe(state_change_cb_t cb, void *ctx);
