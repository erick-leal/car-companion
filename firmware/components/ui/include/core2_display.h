#pragma once
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/**
 * core2_display — driver de pantalla del M5Stack Core2 (ILI9342C por SPI,
 * 320x240). Requiere que core2_power_init() ya haya corrido (alimenta el
 * panel y pulsa su reset via el AXP192 antes de que esto le hable por SPI).
 */

#define CORE2_LCD_WIDTH  320
#define CORE2_LCD_HEIGHT 240

/**
 * Inicializa bus SPI + panel, lo enciende y lo deja listo para dibujar.
 * out_io se necesita para que el caller pueda registrar el callback de
 * "transferencia de color terminada" (ver nota en ui.c: sin eso, LVGL no
 * se entera cuando el SPI realmente termino de mandar los datos).
 */
esp_err_t core2_display_init(esp_lcd_panel_handle_t *out_panel, esp_lcd_panel_io_handle_t *out_io);
