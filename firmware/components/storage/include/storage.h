#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * storage — persistencia local del historial de viajes, antes de que
 * `connectivity` (Fase 2, todavia no escrito) lo sincronice con el backend.
 *
 * Monta una particion FAT dedicada (ver firmware/partitions.csv) y arma los
 * viajes solo (se suscribe a state_store internamente): un viaje "empieza"
 * cuando el motor pasa a tener RPM>0 con datos validos del OBD, y "termina"
 * cuando el OBD se desconecta o el RPM queda en 0 sostenido por
 * TRIP_END_IDLE_TIMEOUT_S (ver storage.c) — para no cortar el viaje cada vez
 * que hay un semaforo en rojo.
 *
 * Limitacion real: sin WiFi/NTP no hay hora de pared confiable, asi que
 * start_time_s es tiempo desde el arranque del ESP32 (esp_timer), no una
 * fecha/hora real.
 *
 * `recorded_at_epoch_s` (agregado 23-24 ago, junto con `connectivity`)
 * guarda ademas la hora real UNIX aproximada al momento de cerrar el viaje,
 * si `connectivity` ya habia sincronizado la hora por SNTP en este arranque
 * (0 si no — viajes de antes de esta funcionalidad, o cerrados antes de la
 * primera sincronizacion SNTP del arranque). Es necesario tenerlo por
 * viaje, no alcanza con calcularlo al sincronizar: el offset de hora real
 * solo es valido para el arranque en el que se calculo, y un viaje puede
 * sincronizarse recien en un arranque posterior al que se grabo (bug real
 * encontrado el 23-24 ago: dos viajes de dias distintos quedaron con la
 * misma fecha en el backend porque se reconstruian con el offset del
 * arranque en que se sincronizaron, no el que se grabaron).
 *
 * BUG DE MIGRACION CONOCIDO: agregar este campo cambio el tamaño del
 * struct, asi que los registros ya guardados en el formato viejo de
 * trips.bin no calzan mas — storage_init() lo detecta (tamaño de archivo no
 * multiplo de sizeof(trip_record_t)) y reinicia trips.bin con un ESP_LOGW,
 * perdiendo el historial viejo. Aceptado a proposito: a esta altura solo
 * habia 2 viajes de prueba.
 */

typedef struct {
    uint32_t start_time_s;      // segundos desde el boot del ESP32 (NO hora de pared, ver nota arriba)
    uint32_t duration_s;
    float    distance_km;       // integrado de velocidad OBD (PID 0x0D) a lo largo del viaje
    float    fuel_used_l;       // integrado de PID 0x5E (caudal instantaneo)
    uint16_t avg_speed_kmh;
    uint16_t max_rpm;
    int16_t  max_coolant_c;
    float    min_battery_v;
    bool     check_engine_seen;    // si el CEL se prendio en algun momento del viaje
    uint32_t recorded_at_epoch_s;  // hora real UNIX aprox. al cerrar el viaje; 0 = desconocida (ver nota arriba)
} trip_record_t;

/**
 * Cada cuantos km toca cambiar el aceite / el filtro. Estimacion de partida
 * para diesel common-rail con aceite semisintetico (uso mixto ciudad/
 * carretera) — NO es un dato del fabricante del Maxus T60. Solo se usan
 * como valor de reset al marcar un cambio hecho (ver storage_mark_*); el
 * contador real (`oil_km_remaining`/`filter_km_remaining`) se puede ajustar
 * a mano en cualquier momento desde la pantalla de Mantenimiento — el
 * odometro propio de este dispositivo arranca en 0 y no tiene forma de
 * saber cuanto le queda realmente a un auto que ya tenia uso antes de
 * instalar esto, asi que el ajuste manual es la unica forma de calibrarlo
 * contra la realidad (pedido real del 22 ago, primera manejada de prueba).
 */
#define STORAGE_OIL_CHANGE_INTERVAL_KM 10000.0f
#define STORAGE_FILTER_CHANGE_INTERVAL_KM 10000.0f

typedef struct {
    float odometer_km;         // suma de distance_km de todos los viajes reales guardados (informativo)
    float oil_km_remaining;    // km que faltan para el proximo cambio de aceite; ajustable a mano
    float filter_km_remaining; // idem para el filtro de aceite, independiente del aceite (no siempre se cambian juntos)
} maintenance_state_t;

esp_err_t storage_init(void);

/**
 * Completa recorded_at_epoch_s (ver trip_record_t arriba) en los viajes ya
 * guardados EN ESTE ARRANQUE que todavia lo tengan en 0 -- llamada por
 * `connectivity` apenas SNTP sincroniza (no hace falta esperar a que haya
 * algo pendiente de sincronizar). Antes de esto, un viaje que terminaba
 * antes de que SNTP sincronizara se quedaba con epoch=0 hasta el momento
 * de sincronizar, y si el dispositivo se reiniciaba en el medio (ej.
 * reflasheos durante pruebas), la fecha reconstruida en ese momento ya no
 * tenia forma de ser correcta (el reloj interno se reinicia en cada
 * arranque). Completarlo apenas se puede, sin esperar el sync, reduce esa
 * ventana de riesgo al minimo. No toca viajes de arranques anteriores (no
 * hay forma de saber si su start_time_s es valido en este arranque).
 */
esp_err_t storage_backfill_recorded_at_epoch(void);

/** Cantidad de viajes guardados en la particion local. */
esp_err_t storage_get_trip_count(uint32_t *out_count);

/** Lee el viaje en la posicion `index` (0 = el mas viejo). */
esp_err_t storage_get_trip(uint32_t index, trip_record_t *out);

/**
 * Cuantos viajes todavia no se mandaron al backend. Los viajes son append-only
 * (nunca se borran de trips.bin), asi que un cursor "cuantos ya se
 * sincronizaron, contando desde el mas viejo" alcanza — pending = total -
 * sincronizados. El cursor se persiste en /storage/sync.bin.
 */
esp_err_t storage_get_pending_sync_count(uint32_t *out_count);

/**
 * Marca los primeros `up_to_count` viajes (indices 0..up_to_count-1, en
 * orden del archivo) como ya sincronizados. `connectivity` la llama despues
 * de un POST exitoso a /api/v1/sync/trips.
 */
esp_err_t storage_mark_trips_synced(uint32_t up_to_count);

/** Estado actual de mantenimiento (odometro + km restantes de aceite y filtro). */
esp_err_t storage_get_maintenance(maintenance_state_t *out);

/** Marca "cambio de aceite hecho ahora": resetea oil_km_remaining al intervalo completo. */
esp_err_t storage_mark_oil_change_done(void);

/** Idem para el filtro. */
esp_err_t storage_mark_filter_change_done(void);

/**
 * Ajusta a mano los km restantes (positivo = suma, negativo = resta) —
 * para calibrar contra el odometro real del auto, que este dispositivo no
 * conoce. Sin limite: un valor negativo es valido y se muestra como
 * "vencido" en la pantalla.
 */
esp_err_t storage_adjust_oil_km_remaining(float delta_km);
esp_err_t storage_adjust_filter_km_remaining(float delta_km);
