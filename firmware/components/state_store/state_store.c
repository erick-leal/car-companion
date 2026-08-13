#include "state_store.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "state_store";
static vehicle_state_t s_state = {0};
// TODO: agregar un mutex (SemaphoreHandle_t) para proteger s_state entre tareas

esp_err_t state_store_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.boost_pressure_kpa = -1;
    ESP_LOGI(TAG, "state_store_init OK");
    return ESP_OK;
}

esp_err_t state_store_get(vehicle_state_t *out)
{
    // TODO: tomar mutex antes de copiar
    memcpy(out, &s_state, sizeof(vehicle_state_t));
    return ESP_OK;
}

esp_err_t state_store_update(const vehicle_state_t *partial)
{
    // TODO: tomar mutex, mergear campos, notificar suscriptores
    ESP_LOGW(TAG, "state_store_update: no implementado todavía");
    return ESP_OK;
}

esp_err_t state_store_subscribe(state_change_cb_t cb, void *ctx)
{
    // TODO: guardar callback en una lista de suscriptores (máx N fijo, sin malloc dinámico)
    ESP_LOGW(TAG, "state_store_subscribe: no implementado todavía");
    return ESP_OK;
}
