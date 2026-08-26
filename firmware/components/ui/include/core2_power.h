#pragma once
#include "esp_err.h"
#include <stdbool.h>

/**
 * core2_power — init minimo del AXP192 (PMIC) del M5Stack Core2.
 *
 * A diferencia del M5Stack clasico (que tiene un pin de backlight comun),
 * el Core2 alimenta la logica y el backlight de la pantalla a traves de
 * rieles del AXP192 (LDO2/LDO3/DCDC3) manejados por I2C — sin este init la
 * pantalla queda completamente apagada aunque el SPI este bien cableado.
 * El reset de la pantalla tambien se hace via un GPIO expandido del AXP192
 * (GPIO4), no un pin directo del ESP32.
 *
 * Valores de registros tomados de la secuencia de bring-up publica del
 * Core2 (AXP192.cpp de M5Stack, usada de la misma forma en M5Unified/M5GFX
 * y en incontables proyectos ESP-IDF/Arduino desde 2020) — no inventados.
 */
esp_err_t core2_power_init(void);

/**
 * Porcentaje de bateria DEL CORE2 (no del auto — para eso esta ATRV via
 * obd_driver). Formula calcada de AXP192::GetBatteryLevel() en
 * https://github.com/m5stack/M5Core2/blob/master/src/AXP192.cpp
 * Devuelve 0-100, o -1 si fallo la lectura I2C.
 */
int core2_power_get_battery_pct(void);

/**
 * Prende/apaga el riel de backlight de la pantalla (DCDC3, ver
 * core2_power_init) sin tocar la logica LCD (LDO2) ni forzar un reset —
 * la pantalla sigue "viva" (LVGL sigue dibujando), solo se apaga la luz de
 * fondo. Usado por `ui` para un standby simple: apagar la pantalla despues
 * de un rato sin conexion OBD ni toques, prenderla de nuevo al primer toque
 * (pedido real del 25 ago: el M5 se calentaba en el bolsillo despues de
 * sacarlo del auto con la pantalla prendida sin nada que mostrar).
 */
esp_err_t core2_power_set_backlight(bool on);
