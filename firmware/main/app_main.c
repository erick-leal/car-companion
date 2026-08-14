/**
 * Car Companion — punto de entrada del firmware.
 *
 * app_main solo arranca las capas en orden y las conecta entre si. No debe
 * tener lógica de negocio (eso vive en cada componente).
 *
 * Orden de arranque (ver docs/architecture.md para el porqué del orden):
 *   1. storage_init()        -> monta almacenamiento local antes que nada más
 *   2. state_store_init()    -> crea el estado compartido (vacío) que todos leen/escriben
 *   3. pid_engine_init()      -> prepara la tabla de PIDs a pollear
 *   4. obd_driver_init()     -> arranca BLE, escanea y conecta al adaptador OBD
 *   5. pid_engine_start_polling() -> empieza a pedir datos apenas haya conexión
 *
 * ui y connectivity todavía no se llaman acá: se integran cuando tengan
 * implementación real (ver TODOs en sus respectivos .c).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "storage.h"
#include "state_store.h"
#include "obd_driver.h"
#include "pid_engine.h"

static const char *TAG = "car_companion";

/* Suscriptor de ejemplo: mientras no exista `ui` real, logueamos por UART
 * cada vez que cambia el estado. Esto es lo que vas a mirar en el monitor
 * serie (`idf.py monitor`) mientras manejas el T60 para el primer test real. */
static void log_state_change(const vehicle_state_t *state, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "RPM=%u  vel=%ukm/h  coolant=%dC  bat=%.1fV  carga=%u%%  MAP=%dkPa  CEL=%s",
             state->rpm, state->speed_kmh, state->coolant_temp_c,
             state->battery_voltage, state->engine_load_pct,
             state->boost_pressure_kpa, state->check_engine_on ? "ON" : "off");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Car Companion — arrancando");

    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(state_store_init());
    ESP_ERROR_CHECK(pid_engine_init());
    ESP_ERROR_CHECK(obd_driver_init());

    state_store_subscribe(log_state_change, NULL);

    ESP_ERROR_CHECK(pid_engine_start_polling());

    ESP_LOGI(TAG, "arranque completo — esperando conexion BLE con el adaptador OBD");

    // TODO: reemplazar este log-only por ui_init() + ui_show_main_screen()
    // apenas el componente `ui` tenga implementación real (ver firmware/components/ui).
}
