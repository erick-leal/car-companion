/**
 * Car Companion — punto de entrada del firmware.
 *
 * app_main solo arranca las capas en orden y las conecta entre si. No debe
 * tener lógica de negocio (eso vive en cada componente).
 *
 * Orden de arranque (ver docs/architecture.md para el porqué del orden):
 *   1. state_store_init()    -> crea el estado compartido (vacío) que todos leen/escriben
 *   2. storage_init()        -> monta almacenamiento local y se suscribe a
 *      state_store para armar el historial de viajes solo — por eso tiene
 *      que ir DESPUES de state_store_init (necesita el mutex ya creado;
 *      llamarlo antes crashea, se probo al reves por error el 22 ago)
 *   3. pid_engine_init()      -> prepara la tabla de PIDs a pollear
 *   4. obd_driver_init()     -> arranca BLE, escanea y conecta al adaptador OBD
 *   5. ui_init()             -> enciende pantalla (AXP192+LVGL) y la deja lista para dibujar
 *   6. connectivity_init()   -> arranca WiFi + tarea de fondo de sync (no
 *      bloqueante: si no hay WiFi, el resto del sistema sigue andando igual)
 *   7. pid_engine_start_polling() -> empieza a pedir datos apenas haya conexión
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "storage.h"
#include "state_store.h"
#include "obd_driver.h"
#include "pid_engine.h"
#include "ui.h"
#include "connectivity.h"

static const char *TAG = "car_companion";

/* Suscriptor de ejemplo: mientras no exista `ui` real, logueamos por UART
 * cada vez que cambia el estado. Esto es lo que vas a mirar en el monitor
 * serie (`idf.py monitor`) mientras manejas el T60 para el primer test real. */
static void log_state_change(const vehicle_state_t *state, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "RPM=%u  vel=%ukm/h  coolant=%dC  bat=%.1fV  carga=%u%%  boost=%dkPa (baro=%ukPa)  "
                  "acel=%u%%  rail=%ukPa  combustible=%.1fL/h  ambiente=%dC  CEL=%s",
             state->rpm, state->speed_kmh, state->coolant_temp_c,
             state->battery_voltage, state->engine_load_pct,
             state->boost_pressure_kpa, state->barometric_pressure_kpa,
             state->throttle_pct, (unsigned)state->fuel_rail_pressure_kpa,
             state->fuel_rate_lph, state->ambient_air_temp_c,
             state->check_engine_on ? "ON" : "off");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Car Companion — arrancando");

    ESP_ERROR_CHECK(state_store_init());
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(pid_engine_init());
    ESP_ERROR_CHECK(obd_driver_init());
    ESP_ERROR_CHECK(ui_init());

    /* NO se envuelve en ESP_ERROR_CHECK a proposito: si WiFi falla al
     * arrancar (o directamente no hay red de casa configurada todavia en
     * connectivity_secrets.h), el dispositivo tiene que seguir funcionando
     * como gauge OBD standalone igual — ese es un requisito real de
     * producto, no un detalle (ver connectivity.h). */
    esp_err_t connectivity_err = connectivity_init();
    if (connectivity_err != ESP_OK) {
        ESP_LOGW(TAG, "connectivity_init fallo (%s) — sigue como gauge standalone, sin sync",
                 esp_err_to_name(connectivity_err));
    }

    state_store_subscribe(log_state_change, NULL);
    ui_show_main_screen();

    ESP_ERROR_CHECK(pid_engine_start_polling());

    ESP_LOGI(TAG, "arranque completo — esperando conexion BLE con el adaptador OBD");
}
