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
#define TRIPS_FILE      MOUNT_POINT "/trips.bin"
#define MAINT_FILE      MOUNT_POINT "/maint.bin"
#define SYNC_FILE       MOUNT_POINT "/sync.bin"
#define CHECKPOINT_FILE MOUNT_POINT "/checkpoint.bin"
#define CLOCK_FILE       MOUNT_POINT "/clock.bin"

/* Cada cuanto se guarda a flash el progreso del viaje en curso, para no
 * perderlo entero si se corta la energia a mitad de un viaje largo (pedido
 * real, ver README: "un viaje en curso vive solo en RAM hasta que termina").
 * 60s, no menos: esto escribe a la MISMA particion FAT+wear-levelling que
 * trips.bin/maint.bin mientras se maneja, asi que el intervalo es un
 * compromiso entre "cuanto se puede llegar a perder" y desgaste de flash --
 * en un viaje de 1h son ~60 escrituras, no miles. */
#define CHECKPOINT_INTERVAL_US (60LL * 1000000)

/* Espejo de los campos de trip_record_t que hacen falta para reconstruirlo
 * despues de un apagado sucio -- no start_time_s (tiempo desde el boot
 * ANTERIOR, ya no significa nada) ni recorded_at_epoch_s del checkpoint en
 * si (se guarda el de INICIO del viaje, igual que trip_record_t). */
typedef struct {
    uint32_t duration_s;
    float    distance_km;
    float    fuel_used_l;
    float    speed_sum;
    uint32_t speed_samples;
    uint16_t max_rpm;
    int16_t  max_coolant_c;
    float    min_battery_v;   // valor crudo, puede ser el sentinel (ver BATTERY_NO_READING_SENTINEL)
    bool     check_engine_seen;
    uint32_t recorded_at_epoch_s;
} trip_checkpoint_t;

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

/* Cuantos viajes ya habia en trips.bin al arrancar -- storage_backfill_
 * recorded_at_epoch_s() solo toca indices >= esto, para no tocar viajes de
 * arranques anteriores (su start_time_s no significa nada en este
 * arranque, ver comentario en storage.h). Fijado una sola vez en
 * storage_init(). */
static uint32_t s_trip_count_at_boot;

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
static int64_t  s_last_checkpoint_us;
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
    s_last_checkpoint_us = now_us;
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

/* Comun a un viaje cerrado normal (end_trip) y a uno recuperado de un
 * checkpoint tras un apagado sucio (recover_checkpoint_if_any) -- ambos
 * terminan igual: append a trips.bin + descontar del odometro/aceite/
 * filtro. */
static void save_trip_and_update_maintenance(const trip_record_t *rec, bool recovered)
{
    FILE *f = fopen(TRIPS_FILE, "ab");
    if (f == NULL) {
        ESP_LOGE(TAG, "no se pudo abrir %s para guardar el viaje (se pierde este viaje)", TRIPS_FILE);
        return;
    }
    size_t written = fwrite(rec, sizeof(*rec), 1, f);
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
    ESP_LOGI(TAG, "viaje %sguardado: %lus, %.1fkm, %.2fL, %uRPM max",
             recovered ? "recuperado de un corte de energia y " : "",
             (unsigned long)rec->duration_s, rec->distance_km, rec->fuel_used_l, rec->max_rpm);

    s_maint.odometer_km += rec->distance_km;
    s_maint.oil_km_remaining -= rec->distance_km;
    s_maint.filter_km_remaining -= rec->distance_km;
    save_maintenance();
}

/* Guarda a flash el progreso acumulado del viaje en curso -- ver
 * CHECKPOINT_INTERVAL_US arriba para cuando se llama. Sobreescribe el
 * checkpoint anterior (no acumula historial, solo el ultimo estado
 * conocido). */
static void write_checkpoint_locked(int64_t now_us)
{
    uint32_t recorded_at_epoch_s = 0;
    int64_t wall_clock_offset_s;
    if (state_store_get_wall_clock_offset(&wall_clock_offset_s)) {
        recorded_at_epoch_s = (uint32_t)(wall_clock_offset_s + s_trip_start_us / 1000000);
    }

    trip_checkpoint_t cp = {
        .duration_s     = (uint32_t)((now_us - s_trip_start_us) / 1000000),
        .distance_km    = (float)s_distance_km_accum,
        .fuel_used_l    = (float)s_fuel_l_accum,
        .speed_sum      = (float)s_speed_sum,
        .speed_samples  = s_speed_samples,
        .max_rpm        = s_max_rpm,
        .max_coolant_c  = s_max_coolant,
        .min_battery_v  = s_min_battery,
        .check_engine_seen = s_check_engine_seen,
        .recorded_at_epoch_s = recorded_at_epoch_s,
    };
    FILE *f = fopen(CHECKPOINT_FILE, "wb");
    if (f == NULL) {
        ESP_LOGW(TAG, "no se pudo abrir %s para guardar el checkpoint del viaje en curso", CHECKPOINT_FILE);
        return;
    }
    size_t written = fwrite(&cp, sizeof(cp), 1, f);
    fclose(f);
    if (written != 1) {
        ESP_LOGW(TAG, "escritura incompleta del checkpoint en %s", CHECKPOINT_FILE);
    }
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
    remove(CHECKPOINT_FILE); // el viaje termino limpio, ya no hace falta recuperarlo en el proximo arranque

    /* Viajes de menos de 5min no se guardan — mover el auto en el garage,
     * probar el motor, etc, no cuentan como viaje real (pedido del 22 ago). */
    if (rec.duration_s < 5 * 60) {
        ESP_LOGI(TAG, "viaje descartado (%lus, muy corto)", (unsigned long)rec.duration_s);
        return;
    }

    save_trip_and_update_maintenance(&rec, false);
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

    if ((now - s_last_checkpoint_us) >= CHECKPOINT_INTERVAL_US) {
        write_checkpoint_locked(now);
        s_last_checkpoint_us = now;
    }

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

/* Si el ultimo apagado paso a mitad de un viaje (corte de energia, panic,
 * etc), end_trip() normal nunca corrio y checkpoint.bin quedo en disco --
 * si hubiera sido un apagado limpio, end_trip() ya lo habria borrado. Es la
 * señal de que hay un viaje que recuperar: se guarda como si hubiera
 * terminado en el ultimo checkpoint conocido, perdiendo como maximo los
 * ~CHECKPOINT_INTERVAL_US segundos previos al corte en vez del viaje
 * entero. */
static void recover_checkpoint_if_any(void)
{
    FILE *f = fopen(CHECKPOINT_FILE, "rb");
    if (f == NULL) return; // apagado limpio la ultima vez, nada que recuperar

    trip_checkpoint_t cp;
    bool ok = fread(&cp, sizeof(cp), 1, f) == 1;
    fclose(f);
    remove(CHECKPOINT_FILE); // se use o no para recuperar, no sirve para el proximo arranque

    if (!ok) {
        ESP_LOGW(TAG, "%s existe pero no se pudo leer completo (corte justo durante esa escritura) -- se descarta", CHECKPOINT_FILE);
        return;
    }

    trip_record_t rec = {
        .start_time_s   = 0, // tiempo desde el boot ANTERIOR, no comparable con esta sesion (ver storage.h)
        .duration_s     = cp.duration_s,
        .distance_km    = cp.distance_km,
        .fuel_used_l    = cp.fuel_used_l,
        .avg_speed_kmh  = cp.speed_samples > 0 ? (uint16_t)(cp.speed_sum / cp.speed_samples) : 0,
        .max_rpm        = cp.max_rpm,
        .max_coolant_c  = cp.max_coolant_c,
        .min_battery_v  = cp.min_battery_v == BATTERY_NO_READING_SENTINEL ? 0.0f : cp.min_battery_v,
        .check_engine_seen = cp.check_engine_seen,
        .recorded_at_epoch_s = cp.recorded_at_epoch_s,
    };

    if (rec.duration_s < 5 * 60) {
        ESP_LOGI(TAG, "checkpoint recuperado descartado (%lus, muy corto)", (unsigned long)rec.duration_s);
        return;
    }

    save_trip_and_update_maintenance(&rec, true);
}

static void load_wall_clock_estimate(void); // definida mas abajo, usada por storage_init()

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

    load_wall_clock_estimate(); // antes que nada mas: cuanto antes tengamos una estimacion, mejor
    migrate_trips_file_if_needed();
    load_maintenance();
    recover_checkpoint_if_any();
    load_sync_state();

    /* Bug real encontrado en el auto (24 ago): el cursor de sync (sync.bin)
     * y trips.bin son archivos independientes -- si trips.bin se reinicia
     * por algun motivo (ver migrate_trips_file_if_needed, o simplemente se
     * borra a mano) sin que sync.bin se entere, el cursor queda "adelantado"
     * respecto al archivo nuevo. Como pending = total - sincronizados, un
     * cursor mayor al total real hace que CUALQUIER viaje nuevo se vea como
     * "ya sincronizado" sin haberse mandado nunca -- exactamente lo que le
     * paso a Erick: el primer viaje real quedo invisible para connectivity,
     * "Nada pendiente" en pantalla, nunca llego un POST al backend (visto
     * en los logs HTTP de Railway). Invariante real: el cursor nunca puede
     * ser mayor al total de viajes guardados, asi que si lo es, es la señal
     * inequivoca de este desincronismo -- se reinicia a 0 (fuerza un
     * resync de los viajes actuales; peor caso duplicar algo que si se
     * habia mandado antes del reinicio del archivo, mejor que perder datos
     * reales en silencio para siempre). */
    uint32_t count = 0;
    storage_get_trip_count(&count);
    if (s_synced_trip_count > count) {
        ESP_LOGW(TAG, "cursor de sync (%lu) mayor que los viajes guardados (%lu) -- "
                      "trips.bin se debe haber reiniciado sin resetear sync.bin, "
                      "reiniciando el cursor a 0",
                 (unsigned long)s_synced_trip_count, (unsigned long)count);
        s_synced_trip_count = 0;
        save_sync_state();
    }

    s_trip_count_at_boot = count;
    state_store_subscribe(on_state_change, NULL);

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

esp_err_t storage_save_wall_clock_estimate(int64_t epoch_s)
{
    if (!storage_lock()) return ESP_ERR_TIMEOUT;
    esp_err_t ret = ESP_OK;
    FILE *f = fopen(CLOCK_FILE, "wb");
    if (f == NULL) {
        ret = ESP_FAIL;
    } else {
        size_t written = fwrite(&epoch_s, sizeof(epoch_s), 1, f);
        fclose(f);
        if (written != 1) ret = ESP_FAIL;
    }
    storage_unlock();
    return ret;
}

/* Llamada una sola vez desde storage_init(), antes de que WiFi/SNTP puedan
 * sincronizar de nuevo en este arranque -- ver storage_save_wall_clock_
 * estimate() en storage.h para el porque. */
static void load_wall_clock_estimate(void)
{
    FILE *f = fopen(CLOCK_FILE, "rb");
    if (f == NULL) return; // primer boot, o SNTP nunca sincronizo en la vida de este dispositivo

    int64_t saved_epoch_s;
    bool ok = fread(&saved_epoch_s, sizeof(saved_epoch_s), 1, f) == 1;
    fclose(f);
    if (!ok) {
        ESP_LOGW(TAG, "%s existe pero no se pudo leer completo, se ignora", CLOCK_FILE);
        return;
    }

    int64_t offset_s = saved_epoch_s - (esp_timer_get_time() / 1000000);
    state_store_set_wall_clock_offset(offset_s);
    ESP_LOGI(TAG, "hora estimada de un arranque anterior cargada de %s (offset provisorio hasta que SNTP sincronice de verdad en este arranque)", CLOCK_FILE);
}

esp_err_t storage_backfill_recorded_at_epoch(void)
{
    int64_t wall_clock_offset_s;
    if (!state_store_get_wall_clock_offset(&wall_clock_offset_s)) {
        return ESP_ERR_INVALID_STATE; // SNTP todavia no sincronizo, nada que completar
    }
    if (!storage_lock()) return ESP_ERR_TIMEOUT;

    uint32_t total;
    get_trip_count_locked(&total);

    FILE *f = fopen(TRIPS_FILE, "r+b"); // r+b: lee y escribe, no trunca (a diferencia de wb)
    if (f == NULL) {
        storage_unlock();
        return ESP_OK; // nada guardado todavia
    }

    uint32_t patched = 0;
    for (uint32_t i = s_trip_count_at_boot; i < total; i++) {
        trip_record_t rec;
        if (fseek(f, (long)(i * sizeof(trip_record_t)), SEEK_SET) != 0 ||
            fread(&rec, sizeof(rec), 1, f) != 1) {
            break; // no deberia pasar (i < total), pero mejor cortar que leer basura
        }
        if (rec.recorded_at_epoch_s != 0) continue; // ya tenia fecha real (SNTP ya habia sincronizado cuando cerro)

        rec.recorded_at_epoch_s = (uint32_t)(wall_clock_offset_s + rec.start_time_s);
        if (fseek(f, (long)(i * sizeof(trip_record_t)), SEEK_SET) != 0 ||
            fwrite(&rec, sizeof(rec), 1, f) != 1) {
            ESP_LOGW(TAG, "no se pudo completar la fecha real del viaje %lu en %s", (unsigned long)i, TRIPS_FILE);
            continue;
        }
        patched++;
    }
    fclose(f);
    storage_unlock();

    if (patched > 0) {
        ESP_LOGI(TAG, "fecha real completada para %lu viaje(s) que habian cerrado antes de que SNTP sincronizara en este arranque", (unsigned long)patched);
    }
    return ESP_OK;
}
