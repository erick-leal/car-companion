/**
 * Car Companion — punto de entrada del firmware.
 *
 * Responsabilidad de app_main: únicamente arrancar las tareas de FreeRTOS
 * de cada capa. No debe contener lógica de negocio.
 *
 * Orden de arranque:
 *   1. storage_init()      -> monta NVS/SD antes que nada más
 *   2. state_store_init()  -> crea el estado compartido (vacío) que todos leen/escriben
 *   3. ui_init()            -> inicializa LVGL y pinta la pantalla de "conectando..."
 *   4. obd_driver_init()   -> intenta conectar por BLE con el adaptador OBD
 *   5. connectivity_init() -> WiFi + OTA (no bloqueante, se conecta en background)
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// TODO: descomentar a medida que se implementen los componentes
// #include "storage.h"
// #include "state_store.h"
// #include "ui.h"
// #include "obd_driver.h"
// #include "connectivity.h"

static const char *TAG = "car_companion";

void app_main(void)
{
    ESP_LOGI(TAG, "Car Companion — arrancando");

    // storage_init();
    // state_store_init();
    // ui_init();
    // obd_driver_init();
    // connectivity_init();

    ESP_LOGI(TAG, "TODO: implementar componentes (ver README de cada carpeta en components/)");
}
