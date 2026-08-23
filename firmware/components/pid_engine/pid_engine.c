#include "pid_engine.h"
#include "pid_math.h"
#include "state_store.h"
#include "obd_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "pid_engine";

#define POLL_INTERVAL_MS 150

static const standard_pid_t s_poll_list[] = {
    PID_ENGINE_RPM,
    PID_VEHICLE_SPEED,
    PID_COOLANT_TEMP,
    PID_ENGINE_LOAD,
    PID_INTAKE_AIR_TEMP,
    PID_INTAKE_MAP,
    PID_MONITOR_STATUS,
    PID_THROTTLE_POS,
    PID_FUEL_RAIL_PRESSURE,
    PID_BAROMETRIC_PRESSURE,
    PID_AMBIENT_AIR_TEMP,
    PID_FUEL_RATE,
};
#define POLL_LIST_LEN (sizeof(s_poll_list) / sizeof(s_poll_list[0]))

static TaskHandle_t s_poll_task_handle;

/* El boost real no es un PID unico: es MAP - presion barometrica local. Se
 * recalcula cada vez que llega cualquiera de los dos, con la barometrica
 * arrancando en un valor de atmosfera estandar hasta la primer lectura real
 * (mejor esa aproximacion que dejar el boost en 0/invalido mientras tanto). */
static uint8_t s_last_map_kpa;
static uint8_t s_last_baro_kpa = 101;

static void update_boost_estimate(void)
{
    state_store_set_boost_pressure((int16_t)s_last_map_kpa - (int16_t)s_last_baro_kpa);
}

esp_err_t pid_engine_init(void)
{
    // TODO: cuando exista pid_table_maxus.c con los PIDs propietarios del T60
    // (ver docs/pid-mapping.md), sumarlos aca a una segunda lista de polling.
    ESP_LOGI(TAG, "pid_engine_init OK (%d PIDs estandar en polling)", (int)POLL_LIST_LEN);
    return ESP_OK;
}

esp_err_t pid_engine_parse_mode01_response(uint8_t pid, const uint8_t *data_bytes, size_t len)
{
    switch (pid) {
        case PID_ENGINE_RPM:
            if (len < 2) return ESP_ERR_INVALID_SIZE;
            return state_store_set_rpm(pid_math_rpm(data_bytes[0], data_bytes[1]));

        case PID_VEHICLE_SPEED:
            if (len < 1) return ESP_ERR_INVALID_SIZE;
            return state_store_set_speed(pid_math_speed_kmh(data_bytes[0]));

        case PID_COOLANT_TEMP:
            if (len < 1) return ESP_ERR_INVALID_SIZE;
            return state_store_set_coolant_temp(pid_math_temp_c(data_bytes[0]));

        case PID_ENGINE_LOAD:
            if (len < 1) return ESP_ERR_INVALID_SIZE;
            return state_store_set_engine_load(pid_math_engine_load_pct(data_bytes[0]));

        case PID_INTAKE_AIR_TEMP:
            if (len < 1) return ESP_ERR_INVALID_SIZE;
            return state_store_set_intake_air_temp(pid_math_temp_c(data_bytes[0]));

        case PID_INTAKE_MAP:
            /* MAP absoluto en kPa. El boost real (MAP - barometrica) se
             * recalcula en update_boost_estimate() combinando esto con
             * PID_BAROMETRIC_PRESSURE — en el motor diesel turbo del T60 es
             * probable que igual no refleje el boost real del turbo y haga
             * falta un PID propietario (ver docs/pid-mapping.md), pero ya es
             * mejor aproximacion que el MAP crudo sin compensar. */
            if (len < 1) return ESP_ERR_INVALID_SIZE;
            s_last_map_kpa = pid_math_map_kpa(data_bytes[0]);
            update_boost_estimate();
            return ESP_OK;

        case PID_BAROMETRIC_PRESSURE:
            if (len < 1) return ESP_ERR_INVALID_SIZE;
            s_last_baro_kpa = pid_math_map_kpa(data_bytes[0]);
            state_store_set_barometric_pressure(s_last_baro_kpa);
            update_boost_estimate();
            return ESP_OK;

        case PID_MONITOR_STATUS:
            if (len < 1) return ESP_ERR_INVALID_SIZE;
            return state_store_set_check_engine(pid_math_mil_on(data_bytes[0]));

        case PID_THROTTLE_POS:
            if (len < 1) return ESP_ERR_INVALID_SIZE;
            return state_store_set_throttle(pid_math_throttle_pct(data_bytes[0]));

        case PID_AMBIENT_AIR_TEMP:
            if (len < 1) return ESP_ERR_INVALID_SIZE;
            return state_store_set_ambient_air_temp(pid_math_temp_c(data_bytes[0]));

        case PID_FUEL_RAIL_PRESSURE:
            if (len < 2) return ESP_ERR_INVALID_SIZE;
            return state_store_set_fuel_rail_pressure(
                pid_math_fuel_rail_pressure_kpa(data_bytes[0], data_bytes[1]));

        case PID_FUEL_RATE:
            if (len < 2) return ESP_ERR_INVALID_SIZE;
            return state_store_set_fuel_rate(pid_math_fuel_rate_lph(data_bytes[0], data_bytes[1]));

        default:
            ESP_LOGW(TAG, "PID 0x%02X sin formula de parseo implementada", pid);
            return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t pid_engine_parse_atrv_response(const char *response_text)
{
    float volts;
    if (!pid_math_parse_atrv(response_text, &volts)) {
        ESP_LOGW(TAG, "respuesta ATRV no parseable: '%s'", response_text);
        return ESP_ERR_INVALID_ARG;
    }
    return state_store_set_battery_voltage(volts);
}

static void handle_obd_response(const uint8_t *raw, size_t raw_len, void *ctx)
{
    standard_pid_t pid = (standard_pid_t)(uintptr_t)ctx;

    char text[64];
    size_t copy_len = raw_len < sizeof(text) - 1 ? raw_len : sizeof(text) - 1;
    memcpy(text, raw, copy_len);
    text[copy_len] = '\0';

    if (strstr(text, "NO DATA") || strstr(text, "ERROR") || strstr(text, "UNABLE")) {
        ESP_LOGW(TAG, "PID 0x%02X: adaptador respondio '%s' (vehiculo no soporta este PID?)", pid, text);
        return;
    }

    uint8_t bytes[16];
    int n = pid_math_ascii_hex_to_bytes(text, bytes, sizeof(bytes));
    if (n < 2 || bytes[0] != 0x41 || bytes[1] != (uint8_t)pid) {
        ESP_LOGW(TAG, "PID 0x%02X: respuesta inesperada '%s'", pid, text);
        return;
    }

    pid_engine_parse_mode01_response(pid, &bytes[2], (size_t)(n - 2));
}

static void handle_atrv_response(const uint8_t *raw, size_t raw_len, void *ctx)
{
    (void)ctx;
    char text[32];
    size_t copy_len = raw_len < sizeof(text) - 1 ? raw_len : sizeof(text) - 1;
    memcpy(text, raw, copy_len);
    text[copy_len] = '\0';
    pid_engine_parse_atrv_response(text);
}

static void handle_dtc_response(const uint8_t *raw, size_t raw_len, void *ctx)
{
    (void)ctx;
    char text[128];
    size_t copy_len = raw_len < sizeof(text) - 1 ? raw_len : sizeof(text) - 1;
    memcpy(text, raw, copy_len);
    text[copy_len] = '\0';

    if (strstr(text, "NO DATA") || strstr(text, "ERROR") || strstr(text, "UNABLE")) {
        ESP_LOGW(TAG, "lectura de DTC: adaptador respondio '%s'", text);
        state_store_set_dtc_read_in_progress(false);
        return;
    }

    uint8_t bytes[32];
    int n = pid_math_ascii_hex_to_bytes(text, bytes, sizeof(bytes));
    if (n < 1) {
        ESP_LOGW(TAG, "lectura de DTC: respuesta no parseable '%s'", text);
        state_store_set_dtc_read_in_progress(false);
        return;
    }

    char codes[STATE_STORE_MAX_DTC][6];
    int count = pid_math_parse_dtc_list(bytes, n, codes, STATE_STORE_MAX_DTC);
    state_store_set_dtc_codes(codes, (uint8_t)count);
    ESP_LOGI(TAG, "lectura de DTC: %d codigo(s) encontrados", count);
}

esp_err_t pid_engine_read_dtc_codes(void)
{
    if (!obd_driver_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    state_store_set_dtc_read_in_progress(true);
    return obd_driver_send_command("03", handle_dtc_response, NULL);
}

/* DEBUG temporal: descubrir que PIDs estandar soporta realmente esta ECU
 * (Maxus T60), en vez de ir probando a ciegas. El modo 01 PID 0x00 devuelve
 * un bitmask de 32 bits con los PIDs 0x01-0x20 soportados; 0x20 idem para
 * 0x21-0x40; 0x40 para 0x41-0x60; 0x60 para 0x61-0x80. Loguea la respuesta
 * cruda nomas (el decodeo del bitmask se hace a mano con el log) — sacar
 * esto una vez que ya sepamos que PIDs agregar de verdad al poll list. */
static void log_discovery_response(const uint8_t *raw, size_t raw_len, void *ctx)
{
    const char *label = (const char *)ctx;
    char text[64];
    size_t copy_len = raw_len < sizeof(text) - 1 ? raw_len : sizeof(text) - 1;
    memcpy(text, raw, copy_len);
    text[copy_len] = '\0';
    ESP_LOGI(TAG, "descubrimiento PIDs soportados (rango %s): '%s'", label, text);
}

static void discover_supported_pids(void)
{
    ESP_LOGI(TAG, "consultando PIDs estandar soportados por la ECU (una sola vez)...");
    obd_driver_send_command("0100", log_discovery_response, (void *)"01-20");
    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    obd_driver_send_command("0120", log_discovery_response, (void *)"21-40");
    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    obd_driver_send_command("0140", log_discovery_response, (void *)"41-60");
    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    obd_driver_send_command("0160", log_discovery_response, (void *)"61-80");
    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
}

static void poll_task(void *arg)
{
    bool discovered = false;
    while (1) {
        if (!obd_driver_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!discovered) {
            discover_supported_pids();
            discovered = true;
        }

        /* Pedido de lectura de DTC desde ui (via el "buzon" de state_store,
         * ver nota en state_store.h). Se atiende ANTES del polling normal de
         * este ciclo para que la pantalla de Fallas no espere una vuelta
         * completa de 12 PIDs (~2s) sin necesidad. */
        if (state_store_consume_dtc_read_request()) {
            pid_engine_read_dtc_codes();
        }

        for (size_t i = 0; i < POLL_LIST_LEN; i++) {
            standard_pid_t pid = s_poll_list[i];
            char cmd[8];
            snprintf(cmd, sizeof(cmd), "01%02X", (uint8_t)pid);
            obd_driver_send_command(cmd, handle_obd_response, (void *)(uintptr_t)pid);
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        }

        obd_driver_send_command("ATRV", handle_atrv_response, NULL);
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t pid_engine_start_polling(void)
{
    if (s_poll_task_handle != NULL) {
        ESP_LOGW(TAG, "pid_engine_start_polling ya estaba corriendo");
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t ok = xTaskCreate(poll_task, "pid_poll", 4096, NULL, 5, &s_poll_task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
