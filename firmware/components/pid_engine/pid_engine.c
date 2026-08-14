#include "pid_engine.h"
#include "pid_math.h"
#include "state_store.h"
#include "obd_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "pid_engine";

#define POLL_INTERVAL_MS 150

static const standard_pid_t s_poll_list[] = {
    PID_ENGINE_RPM,
    PID_VEHICLE_SPEED,
    PID_COOLANT_TEMP,
    PID_ENGINE_LOAD,
    PID_INTAKE_AIR_TEMP,
    PID_INTAKE_MAP,
};
#define POLL_LIST_LEN (sizeof(s_poll_list) / sizeof(s_poll_list[0]))

static TaskHandle_t s_poll_task_handle;

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
            /* MAP absoluto en kPa, NO es "presion de boost" directamente:
             * boost real = MAP - presion atmosferica local. Ademas, en el
             * motor diesel turbo del T60 es probable que este PID estandar
             * no refleje bien el boost real y haga falta un PID propietario
             * (ver docs/pid-mapping.md). Se guarda igual como aproximacion. */
            if (len < 1) return ESP_ERR_INVALID_SIZE;
            return state_store_set_boost_pressure((int16_t)pid_math_map_kpa(data_bytes[0]));

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

static void poll_task(void *arg)
{
    while (1) {
        if (!obd_driver_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
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
