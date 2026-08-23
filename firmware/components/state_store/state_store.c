#include "state_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "state_store";

static vehicle_state_t s_state;
static SemaphoreHandle_t s_mutex;

typedef struct {
    state_change_cb_t cb;
    void *ctx;
    bool used;
} subscriber_t;

static subscriber_t s_subscribers[STATE_STORE_MAX_SUBSCRIBERS];

/* Tiempo de espera al tomar el mutex. Si esto se agota en la práctica, hay un
 * bug real de contención en algún lado (una tarea que se queda con el lock
 * demasiado tiempo, ej. dibujando LVGL con el mutex tomado) — no subir este
 * valor a "lo que sea" para silenciar el problema. */
#define LOCK_TIMEOUT_TICKS pdMS_TO_TICKS(100)

static esp_err_t lock(void)
{
    if (xSemaphoreTake(s_mutex, LOCK_TIMEOUT_TICKS) != pdTRUE) {
        ESP_LOGE(TAG, "timeout esperando el mutex de state_store");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static void notify_subscribers(void)
{
    /* Copiamos el estado y soltamos el mutex ANTES de llamar a los callbacks:
     * si un callback (ej. de ui) termina llamando de vuelta a state_store_get,
     * un mutex no-reentrante se trabaría (deadlock). */
    vehicle_state_t snapshot;
    if (lock() != ESP_OK) {
        return;
    }
    memcpy(&snapshot, &s_state, sizeof(snapshot));
    unlock();

    for (int i = 0; i < STATE_STORE_MAX_SUBSCRIBERS; i++) {
        if (s_subscribers[i].used) {
            s_subscribers[i].cb(&snapshot, s_subscribers[i].ctx);
        }
    }
}

esp_err_t state_store_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.boost_pressure_kpa = INT16_MIN;
    memset(s_subscribers, 0, sizeof(s_subscribers));

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "no se pudo crear el mutex");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "state_store_init OK");
    return ESP_OK;
}

esp_err_t state_store_get(vehicle_state_t *out)
{
    esp_err_t err = lock();
    if (err != ESP_OK) {
        return err;
    }
    memcpy(out, &s_state, sizeof(vehicle_state_t));
    unlock();
    return ESP_OK;
}

esp_err_t state_store_set_disconnected(void)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.data_valid = false;
    s_state.dtc_read_in_progress = false;  // no vamos a recibir esa respuesta si estaba en vuelo
    s_state.dtc_clear_in_progress = false;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_subscribe(state_change_cb_t cb, void *ctx)
{
    for (int i = 0; i < STATE_STORE_MAX_SUBSCRIBERS; i++) {
        if (!s_subscribers[i].used) {
            s_subscribers[i].cb = cb;
            s_subscribers[i].ctx = ctx;
            s_subscribers[i].used = true;
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "no hay slots libres de suscriptor (max %d)", STATE_STORE_MAX_SUBSCRIBERS);
    return ESP_ERR_NO_MEM;
}

/* --- Setters ---
 * Todos siguen el mismo patrón: lock -> escribir campo + timestamp + data_valid -> unlock -> notificar.
 * Se repite el patrón a propósito (en vez de una macro) porque el compilador te avisa
 * de inmediato si cambia el tipo de algún campo; una macro genérica lo esconde. */

esp_err_t state_store_set_rpm(uint16_t rpm)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.rpm = rpm;
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_speed(uint8_t speed_kmh)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.speed_kmh = speed_kmh;
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_coolant_temp(int16_t temp_c)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.coolant_temp_c = temp_c;
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_battery_voltage(float volts)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.battery_voltage = volts;
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_boost_pressure(int16_t kpa)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.boost_pressure_kpa = kpa;
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_engine_load(uint8_t load_pct)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.engine_load_pct = load_pct;
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_intake_air_temp(int16_t temp_c)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.intake_air_temp_c = temp_c;
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_check_engine(bool on)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.check_engine_on = on;
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_throttle(uint8_t pct)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.throttle_pct = pct;
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_fuel_rail_pressure(uint32_t kpa)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.fuel_rail_pressure_kpa = kpa;
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_fuel_rate(float lph)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.fuel_rate_lph = lph;
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_ambient_air_temp(int16_t temp_c)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.ambient_air_temp_c = temp_c;
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_barometric_pressure(uint8_t kpa)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.barometric_pressure_kpa = kpa;
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_dtc_codes(const char codes[][6], uint8_t count)
{
    if (count > STATE_STORE_MAX_DTC) {
        count = STATE_STORE_MAX_DTC; // recortar en vez de fallar: mejor mostrar 8 de 9 que nada
    }
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    memset(s_state.dtc_codes, 0, sizeof(s_state.dtc_codes));
    for (uint8_t i = 0; i < count; i++) {
        memcpy(s_state.dtc_codes[i], codes[i], 6);
    }
    s_state.dtc_count = count;
    s_state.dtc_read_in_progress = false;
    s_state.dtc_clear_in_progress = false; // una lectura exitosa tambien cierra un "Borrando..." si quedo pendiente
    s_state.data_valid = true;
    s_state.last_update_us = esp_timer_get_time();
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_dtc_read_in_progress(bool in_progress)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.dtc_read_in_progress = in_progress;
    unlock();
    notify_subscribers();
    return ESP_OK;
}

esp_err_t state_store_set_dtc_clear_in_progress(bool in_progress)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_state.dtc_clear_in_progress = in_progress;
    unlock();
    notify_subscribers();
    return ESP_OK;
}

/* No son parte de vehicle_state_t a proposito: son pedidos de accion, no un
 * dato del vehiculo, asi que no disparan notify_subscribers (nadie necesita
 * "redibujar" por esto, solo pid_engine consumiendolos una vez). */
static bool s_dtc_read_requested;
static bool s_dtc_clear_requested;

esp_err_t state_store_request_dtc_read(void)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_dtc_read_requested = true;
    unlock();
    return ESP_OK;
}

bool state_store_consume_dtc_read_request(void)
{
    if (lock() != ESP_OK) return false;
    bool was_requested = s_dtc_read_requested;
    s_dtc_read_requested = false;
    unlock();
    return was_requested;
}

esp_err_t state_store_request_dtc_clear(void)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_dtc_clear_requested = true;
    unlock();
    return ESP_OK;
}

bool state_store_consume_dtc_clear_request(void)
{
    if (lock() != ESP_OK) return false;
    bool was_requested = s_dtc_clear_requested;
    s_dtc_clear_requested = false;
    unlock();
    return was_requested;
}

static bool s_sync_requested;

esp_err_t state_store_request_sync(void)
{
    esp_err_t err = lock();
    if (err != ESP_OK) return err;
    s_sync_requested = true;
    unlock();
    return ESP_OK;
}

bool state_store_consume_sync_request(void)
{
    if (lock() != ESP_OK) return false;
    bool was_requested = s_sync_requested;
    s_sync_requested = false;
    unlock();
    return was_requested;
}
