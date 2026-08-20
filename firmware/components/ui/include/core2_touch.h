#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * core2_touch — panel tactil capacitivo del M5Stack Core2 (FT6336U por I2C).
 *
 * Comparte el bus I2C con el AXP192, asi que core2_power_init() tiene que
 * haber corrido antes (es quien instala el driver de I2C). Este modulo solo
 * lee registros, no reinstala el bus.
 *
 * El panel tactil del Core2 es mas alto que el area visible: reporta hasta
 * y~280, donde la franja y>240 son los tres circulos serigrafiados debajo de
 * la pantalla (los "botones" A/B/C del Core2). Aca devolvemos la coordenada
 * cruda; quien la use decide que hacer con esa franja.
 */

esp_err_t core2_touch_init(void);

/**
 * Lee el estado actual del tactil. Devuelve true si hay un dedo apoyado, y
 * en ese caso escribe la posicion cruda (sin transformar) en out_x/out_y.
 */
bool core2_touch_read(uint16_t *out_x, uint16_t *out_y);
