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
 * fecha/hora real. Cuando `connectivity` sincronice con el backend, ahi se
 * puede resolver la fecha real (el backend sabe cuando llego cada sync).
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
    bool     check_engine_seen; // si el CEL se prendio en algun momento del viaje
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

/** Cantidad de viajes guardados en la particion local. */
esp_err_t storage_get_trip_count(uint32_t *out_count);

/** Lee el viaje en la posicion `index` (0 = el mas viejo). */
esp_err_t storage_get_trip(uint32_t index, trip_record_t *out);

/**
 * Cuantos viajes todavia no se mandaron al backend. Hoy (sin `connectivity`
 * implementado) es simplemente el total, porque nunca se sincronizo nada
 * — la funcion ya existe para que connectivity la use tal cual cuando este.
 */
esp_err_t storage_get_pending_sync_count(uint32_t *out_count);

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
