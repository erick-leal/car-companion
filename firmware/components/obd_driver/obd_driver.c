#include "obd_driver.h"
#include "esp_log.h"

static const char *TAG = "obd_driver";
static bool s_connected = false;

esp_err_t obd_driver_init(void)
{
    // TODO:
    // 1. Inicializar BLE (NimBLE) en modo central.
    // 2. Escanear y conectar al Vgate vLinker MC+ (filtrar por nombre o UUID de servicio).
    // 3. Enviar secuencia de init ELM327: ATZ, ATE0, ATL0, ATSP0 (auto-detectar protocolo).
    // 4. Confirmar con "0100" que el adaptador responde PIDs soportados.
    ESP_LOGW(TAG, "obd_driver_init: no implementado todavía");
    return ESP_OK;
}

esp_err_t obd_driver_send_command(const char *pid_hex, obd_response_cb_t cb, void *ctx)
{
    // TODO: escribir sobre la característica BLE correspondiente y esperar respuesta
    // asíncrona (el ELM327 responde con "> " como prompt de fin de respuesta).
    ESP_LOGW(TAG, "obd_driver_send_command(%s): no implementado todavía", pid_hex);
    return ESP_OK;
}

bool obd_driver_is_connected(void)
{
    return s_connected;
}
