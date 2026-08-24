#include "storage.h"
#include "state_store.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "storage";

#define MOUNT_POINT   "/storage"
#define TRIPS_FILE    MOUNT_POINT "/trips.bin"
#define MAINT_FILE    MOUNT_POINT "/maint.bin"
#define SYNC_FILE     MOUNT_POINT "/sync.bin"

/* Con el motor en 0 RPM por mas de esto, se da el viaje por terminado. 15min
 * (no 2) porque una parada a cargar combustible + comprar algo facil entra
 * en ese rango, y no queremos que eso corte el viaje en dos (pedido real del
 * 22 ago, pensando en paradas tipo Copec). */
#define TRIP_END_IDLE_TIMEOUT_S (15 * 60)

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

/* BUG REAL DE CONCURRENCIA (encontrado en auditoria, 24 ago): on_state_change
 * corre en la tarea del host de NimBLE (via los setters normales de PIDs) Y
 * TAMBIEN en la tarea de pid_engine (via state_store_set_disconnected(), ver
 * poll_task en pid_engine.c) — sin ningun mutex, dos llamadas casi
 * simultaneas podian pasar juntas el chequeo `if (!s_in_trip)` y guardar el
 * mismo viaje dos veces, o pisarse escribiendo maint.bin/trips.bin al mismo
 * tiempo (FATFS no tiene lock propio en este proyecto, CONFIG_FATFS_FS_LOCK=0).
 * Un solo mutex protege todo el estado compartido de este archivo: el viaje
 * en curso, s_maint, y el cursor de sync. */
static SemaphoreHandle_t s_mutex;

/* Timeout generoso (no como el de state_store) porque ademas de memoria esto
 * puede estar esperando E/S real de flash (fopen/fwrite/fclose sobre
 * FATFS+wear-levelling). Si esto se agota en la practica, hay una tarea
 * quedandose con el lock mucho mas de lo esperado — investigar, no subir
 * el timeout para silenciarlo. */
#define STORAGE_LOCK_TIMEOUT_TICKS pdMS_TO_TICKS(500)

static bool storage_lock(void)
{
    if (xSemaphoreTake(s_mutex, STORAGE_LOCK_TIMEOUT_TICKS) != pdTRUE) {
        ESP_LOGE(TAG, "timeout esperando el mutex de storage (500ms) — alguna operacion de flash se esta demorando de mas");
        return false;
    }
    return true;
}

static void storage_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

/* Cache en RAM del estado de mantenimiento, con escritura directa a flash en
 * cada cambio (se actualiza poco: al cerrar un viaje real y al marcar un
 * cambio de aceite, no en cada muestra del OBD como los demas datos). */
static maintenance_state_t s_maint;

/* Valores de arranque cuando no hay (o no se puede leer) un mantenimiento
 * guardado: odometro en 0, pero los contadores de aceite/filtro arrancan en
 * el intervalo completo (no en 0 — 0 se veria como "VENCIDO", que no es
 * cierto en un dispositivo recien instalado). */
static void reset_maintenance_to_defaults(void)
{
    memset(&s_maint, 0, sizeof(s_maint));
    s_maint.oil_km_remaining = STORAGE_OIL_CHANGE_INTERVAL_KM;
    s_maint.filter_km_remaining = STORAGE_FILTER_CHANGE_INTERVAL_KM;
}

static void load_maintenance(void)
{
    FILE *f = fopen(MAINT_FILE, "rb");
    if (f == NULL) {
        reset_maintenance_to_defaults(); // primer boot: sin historial
        return;
    }
    if (fread(&s_maint, sizeof(s_maint), 1, f) != 1) {
        /* Archivo existe pero no se pudo leer completo — corrupcion real, o
         * (mas probable la primera vez que se ve este log) el formato viejo
         * de este archivo tenia menos campos que el actual y ya no calza en
         * tamaño. En ambos casos se pierde el mantenimiento guardado, pero
         * mejor eso que arrancar con datos a medio leer. */
        ESP_LOGW(TAG, "%s existe pero no se pudo leer completo (formato viejo o corrupcion), reiniciando mantenimiento", MAINT_FILE);
        reset_maintenance_to_defaults();
    }
    fclose(f);
}

static void save_maintenance(void)
{
    FILE *f = fopen(MAINT_FILE, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "no se pudo abrir %s para guardar mantenimiento", MAINT_FILE);
        return;
    }
    size_t written = fwrite(&s_maint, sizeof(s_maint), 1, f);
    fclose(f);
    if (written != 1) {
        ESP_LOGE(TAG, "escritura incompleta de %s (el odometro/ultimo cambio puede haber quedado desactualizado)", MAINT_FILE);
    }
}

/* Cursor de sincronizacion: cuantos viajes (contando desde el mas viejo, en
 * orden de trips.bin) ya se mandaron al backend con exito. Cache en RAM,
 * escritura directa a flash en cada cambio (se actualiza poco: solo despues
 * de un POST exitoso de connectivity). */
static uint32_t s_synced_trip_count;

static void load_sync_state(void)
{
    FILE *f = fopen(SYNC_FILE, "rb");
    if (f == NULL) {
        s_synced_trip_count = 0; // primer boot, o connectivity nunca sincronizo nada todavia
        return;
    }
    if (fread(&s_synced_trip_count, sizeof(s_synced_trip_count), 1, f) != 1) {
        ESP_LOGW(TAG, "%s existe pero no se pudo leer completo, reiniciando cursor de sync en 0", SYNC_FILE);
        s_synced_trip_count = 0;
    }
    fclose(f);
}

static void save_sync_state(void)
{
    FILE *f = fopen(SYNC_FILE, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "no se pudo abrir %s para guardar el cursor de sync", SYNC_FILE);
        return;
    }
    size_t written = fwrite(&s_synced_trip_count, sizeof(s_synced_trip_count), 1, f);
    fclose(f);
    if (written != 1) {
        ESP_LOGE(TAG, "escritura incompleta de %s (puede re-sincronizar viajes ya mandados)", SYNC_FILE);
    }
}

/* --- Estado del viaje en curso (RAM, se persiste recien al terminar) --- */
static bool     s_in_trip;
static int64_t  s_trip_start_us;
static int64_t  s_last_sample_us;   // 0 = todavia no hubo una muestra real para integrar contra
static int64_t  s_last_rpm_nonzero_us;
static double   s_distance_km_accum;
static double   s_fuel_l_accum;
static double   s_speed_sum;
static uint32_t s_speed_samples;
static uint16_t s_max_rpm;
static int16_t  s_max_coolant;
static float    s_min_battery;
static bool     s_check_engine_seen;

/* BUG REAL ENCONTRADO EN MANEJO (22 ago): los dos primeros viajes de prueba
 * mostraron "Bateria min: 0.0V" siempre, imposible con el auto andando (si
 * el ECU responde al OBD, tiene que tener bateria). Causa: la bateria se lee
 * por ATRV una vez por vuelta de polling (al final, ver pid_engine.c), asi
 * que battery_voltage en state_store puede seguir en su default de 0.0
 * (struct recien inicializado) el instante exacto en que arranca el viaje
 * (dispara con el primer RPM>0, que llega ANTES que la primera respuesta de
 * ATRV en esa vuelta). Una vez que s_min_battery quedaba en 0.0, ningun
 * valor real (12-14V) es "menor", asi que se quedaba pegado ahi para
 * siempre. Se trata 0.0V como "sin lectura todavia" (mismo criterio que ya
 * se usa en otros lados del codebase, ej. boost_pressure_kpa con
 * INT16_MIN) en vez de un valor real a comparar. */
#define BATTERY_NO_READING_SENTINEL 999.0f

static void start_trip(int64_t now_us, const vehicle_state_t *state)
{
    s_in_trip = true;
    s_trip_start_us = now_us;
    s_last_sample_us = 0;
    s_distance_km_accum = 0;
    s_fuel_l_accum = 0;
    s_speed_sum = 0;
    s_speed_samples = 0;
    s_max_rpm = state->rpm;
    s_max_coolant = state->coolant_temp_c;
    s_min_battery = state->battery_voltage > 0.0f ? state->battery_voltage : BATTERY_NO_READING_SENTINEL;
    s_check_engine_seen = state->check_engine_on;
    ESP_LOGI(TAG, "viaje iniciado");
}

static void end_trip(int64_t now_us)
{
    /* Hora real aproximada de inicio del viaje, si connectivity ya
     * sincronizo por SNTP en este arranque (ver nota grande en
     * storage.h/trip_record_t) — 0 si no, se resuelve como mejor se pueda
     * al sincronizar. */
    uint32_t recorded_at_epoch_s = 0;
    int64_t wall_clock_offset_s;
    if (state_store_get_wall_clock_offset(&wall_clock_offset_s)) {
        recorded_at_epoch_s = (uint32_t)(wall_clock_offset_s + s_trip_start_us / 1000000);
    }

    trip_record_t rec = {
        .start_time_s   = (uint32_t)(s_trip_start_us / 1000000),
        .duration_s     = (uint32_t)((now_us - s_trip_start_us) / 1000000),
        .distance_km    = (float)s_distance_km_accum,
        .fuel_used_l    = (float)s_fuel_l_accum,
        .avg_speed_kmh  = s_speed_samples > 0 ? (uint16_t)(s_speed_sum / s_speed_samples) : 0,
        .max_rpm        = s_max_rpm,
        .max_coolant_c  = s_max_coolant,
        /* Si nunca llego una lectura real de bateria en todo el viaje, se
         * guarda 0.0 -- mismo "0 = sin dato" que ya usan otros campos del
         * struct (ver storage.h/trip_record_t), no un valor real medido. */
        .min_battery_v  = s_min_battery == BATTERY_NO_READING_SENTINEL ? 0.0f : s_min_battery,
        .check_engine_seen = s_check_engine_seen,
        .recorded_at_epoch_s = recorded_at_epoch_s,
    };
    s_in_trip = false;

    /* Viajes de menos de 5min no se guardan — mover el auto en el garage,
     * probar el motor, etc, no cuentan como viaje real (pedido del 22 ago). */
    if (rec.duration_s < 5 * 60) {
        ESP_LOGI(TAG, "viaje descartado (%lus, muy corto)", (unsigned long)rec.duration_s);
        return;
    }

    FILE *f = fopen(TRIPS_FILE, "ab");
    if (f == NULL) {
        ESP_LOGE(TAG, "no se pudo abrir %s para guardar el viaje (se pierde este viaje)", TRIPS_FILE);
        return;
    }
    size_t written = fwrite(&rec, sizeof(rec), 1, f);
    fclose(f);
    if (written != 1) {
        /* Visto en la practica cuando la particion se queda sin espacio o hay
         * un error de flash real — mejor loguearlo fuerte que dejar un
         * registro a medio escribir en silencio (trips.bin se lee despues
         * por offset fijo, un registro corrupto correria todos los
         * siguientes). No reintentamos: si fallo por espacio, reintentar no
         * arregla nada. */
        ESP_LOGE(TAG, "escritura incompleta del viaje en %s (se pierde este viaje)", TRIPS_FILE);
        return;
    }
    ESP_LOGI(TAG, "viaje guardado: %lus, %.1fkm, %.2fL, %uRPM max",
             (unsigned long)rec.duration_s, rec.distance_km, rec.fuel_used_l, rec.max_rpm);

    s_maint.odometer_km += rec.distance_km;
    s_maint.oil_km_remaining -= rec.distance_km;
    s_maint.filter_km_remaining -= rec.distance_km;
    save_maintenance();
}

static void on_state_change(const vehicle_state_t *state, void *ctx)
{
    (void)ctx;
    if (!storage_lock()) return; // se pierde esta actualizacion puntual antes que corromper un archivo
    int64_t now = esp_timer_get_time();

    if (!state->data_valid) {
        if (s_in_trip) end_trip(now);
        storage_unlock();
        return;
    }

    if (state->rpm > 0) {
        s_last_rpm_nonzero_us = now;
        if (!s_in_trip) start_trip(now, state);
    }

    if (!s_in_trip) {
        storage_unlock();
        return;
    }

    /* Integrar distancia/combustible contra el tiempo real transcurrido
     * desde la ultima muestra. Se descarta un salto de tiempo grande (mas de
     * 30s entre muestras, cuando normalmente llegan cada ~150ms) para no
     * inventar distancia/combustible de un hueco de datos (reconexion BLE,
     * etc) como si el auto hubiera viajado durante ese hueco. */
    if (s_last_sample_us != 0) {
        double dt_hours = (now - s_last_sample_us) / 3600000000.0;
        if (dt_hours > 0 && dt_hours < (30.0 / 3600.0)) {
            s_distance_km_accum += state->speed_kmh * dt_hours;
            s_fuel_l_accum += state->fuel_rate_lph * dt_hours;
        }
    }
    s_last_sample_us = now;

    s_speed_sum += state->speed_kmh;
    s_speed_samples++;
    if (state->rpm > s_max_rpm) s_max_rpm = state->rpm;
    if (state->coolant_temp_c > s_max_coolant) s_max_coolant = state->coolant_temp_c;
    if (state->battery_voltage > 0.0f && state->battery_voltage < s_min_battery) s_min_battery = state->battery_voltage;
    if (state->check_engine_on) s_check_engine_seen = true;

    if ((now - s_last_rpm_nonzero_us) > (int64_t)TRIP_END_IDLE_TIMEOUT_S * 1000000) {
        end_trip(now);
    }
    storage_unlock();
}

/* trip_record_t gano un campo nuevo el 23-24 ago (recorded_at_epoch_s), lo
 * que cambio su tamaño en bytes — un trips.bin grabado con el struct viejo
 * ya no calza en offsets fijos de index*sizeof(trip_record_t). Se detecta
 * por el tamaño del archivo no siendo multiplo del tamaño ACTUAL del
 * struct, y se reinicia entero: no hay forma segura de reinterpretar bytes
 * en un formato que ya no existe en el codigo. Perdida de datos aceptada a
 * proposito (solo habia 2 viajes de prueba en ese momento). */
static void migrate_trips_file_if_needed(void)
{
    FILE *f = fopen(TRIPS_FILE, "rb");
    if (f == NULL) return; // no existe todavia, nada que migrar
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    if (size < 0 || (size % (long)sizeof(trip_record_t)) == 0) {
        return; // formato actual, o archivo vacio/corrupto de otra forma (no es este caso)
    }

    ESP_LOGW(TAG, "%s tiene un tamaño (%ld bytes) que no calza con el formato actual de trip_record_t "
                  "(%u bytes) -- probablemente quedo del formato viejo antes de recorded_at_epoch_s. "
                  "Reiniciando el historial de viajes (no se puede leer con seguridad).",
             TRIPS_FILE, size, (unsigned)sizeof(trip_record_t));
    FILE *wf = fopen(TRIPS_FILE, "wb"); // "wb" trunca a 0 bytes
    if (wf != NULL) fclose(wf);
}

esp_err_t storage_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "no se pudo crear el mutex de storage");
        return ESP_ERR_NO_MEM;
    }

    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 4,
        .format_if_mount_failed = true, // primer boot: la particion viene sin formatear
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_POINT, "storage", &mount_config, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo montar la particion de storage: %s", esp_err_to_name(err));
        return err;
    }

    migrate_trips_file_if_needed();
    load_maintenance();
    load_sync_state();
    state_store_subscribe(on_state_change, NULL);

    uint32_t count = 0;
    storage_get_trip_count(&count);
    ESP_LOGI(TAG, "storage_init OK (%s montado, %lu viajes guardados, %lu sin sincronizar, odometro %.1fkm)",
             MOUNT_POINT, (unsigned long)count,
             (unsigned long)(count > s_synced_trip_count ? count - s_synced_trip_count : 0),
             s_maint.odometer_km);
    return ESP_OK;
}

/* Helpers "_locked": asumen que el llamador YA tiene el mutex. Existen para
 * que storage_get_pending_sync_count pueda reusar la logica de contar sin
 * volver a tomar el mutex (un mutex normal de FreeRTOS no es reentrante —
 * tomarlo dos veces desde la misma tarea se traba). */
static esp_err_t get_trip_count_locked(uint32_t *out_count)
{
    FILE *f = fopen(TRIPS_FILE, "rb");
    if (f == NULL) {
        *out_count = 0; // no hay archivo todavia = ningun viaje guardado, no es un error
        return ESP_OK;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    *out_count = (uint32_t)(size / (long)sizeof(trip_record_t));
    return ESP_OK;
}

esp_err_t storage_get_trip_count(uint32_t *out_count)
{
    if (!storage_lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = get_trip_count_locked(out_count);
    storage_unlock();
    return err;
}

esp_err_t storage_get_trip(uint32_t index, trip_record_t *out)
{
    if (!storage_lock()) return ESP_ERR_TIMEOUT;
    FILE *f = fopen(TRIPS_FILE, "rb");
    if (f == NULL) {
        storage_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t ret = ESP_OK;
    if (fseek(f, (long)(index * sizeof(trip_record_t)), SEEK_SET) != 0 ||
        fread(out, sizeof(*out), 1, f) != 1) {
        ret = ESP_ERR_NOT_FOUND;
    }
    fclose(f);
    storage_unlock();
    return ret;
}

esp_err_t storage_get_pending_sync_count(uint32_t *out_count)
{
    if (!storage_lock()) return ESP_ERR_TIMEOUT;
    uint32_t total;
    get_trip_count_locked(&total);
    *out_count = total > s_synced_trip_count ? total - s_synced_trip_count : 0;
    storage_unlock();
    return ESP_OK;
}

esp_err_t storage_mark_trips_synced(uint32_t up_to_count)
{
    if (!storage_lock()) return ESP_ERR_TIMEOUT;
    if (up_to_count > s_synced_trip_count) {
        s_synced_trip_count = up_to_count;
        save_sync_state();
    }
    storage_unlock();
    return ESP_OK;
}

esp_err_t storage_get_maintenance(maintenance_state_t *out)
{
    if (!storage_lock()) return ESP_ERR_TIMEOUT;
    *out = s_maint;
    storage_unlock();
    return ESP_OK;
}

esp_err_t storage_mark_oil_change_done(void)
{
    if (!storage_lock()) return ESP_ERR_TIMEOUT;
    s_maint.oil_km_remaining = STORAGE_OIL_CHANGE_INTERVAL_KM;
    save_maintenance();
    storage_unlock();
    ESP_LOGI(TAG, "cambio de aceite marcado (odometro propio: %.1fkm)", s_maint.odometer_km);
    return ESP_OK;
}

esp_err_t storage_mark_filter_change_done(void)
{
    if (!storage_lock()) return ESP_ERR_TIMEOUT;
    s_maint.filter_km_remaining = STORAGE_FILTER_CHANGE_INTERVAL_KM;
    save_maintenance();
    storage_unlock();
    ESP_LOGI(TAG, "cambio de filtro marcado (odometro propio: %.1fkm)", s_maint.odometer_km);
    return ESP_OK;
}

esp_err_t storage_adjust_oil_km_remaining(float delta_km)
{
    if (!storage_lock()) return ESP_ERR_TIMEOUT;
    s_maint.oil_km_remaining += delta_km;
    save_maintenance();
    storage_unlock();
    return ESP_OK;
}

esp_err_t storage_adjust_filter_km_remaining(float delta_km)
{
    if (!storage_lock()) return ESP_ERR_TIMEOUT;
    s_maint.filter_km_remaining += delta_km;
    save_maintenance();
    storage_unlock();
    return ESP_OK;
}
