#include "core2_display.h"
#include "esp_lcd_ili9341.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "core2_display";

/* Pines SPI de la pantalla del Core2 (no comparte pines con el touch, que es I2C). */
#define LCD_SPI_HOST  SPI2_HOST
#define PIN_MOSI      23
#define PIN_SCLK      18
#define PIN_CS        5
#define PIN_DC        15
/* Sin pin de RST directo: el reset del panel se hace via GPIO4 del AXP192,
 * ya pulsado en core2_power_init(). Por eso -1 aca (esp_lcd no toca ningun
 * pin de reset propio). */
#define PIN_RST       (-1)

esp_err_t core2_display_init(esp_lcd_panel_handle_t *out_panel, esp_lcd_panel_io_handle_t *out_io)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CORE2_LCD_WIDTH * 80 * sizeof(uint16_t), // suficiente para ~80 lineas por transferencia
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize fallo: %d", err);
        return err;
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi fallo: %d", err);
        return err;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, // el ILI9342C del Core2 esta cableado en orden BGR
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_ili9341 fallo: %d", err);
        return err;
    }

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true); // tipico del panel del Core2

    /* NO usar swap_xy aca, aunque el driver que reusamos sea el del ILI9341.
     *
     * El Core2 monta un ILI9342C, que es NATIVAMENTE 320x240 (horizontal) —
     * confirmado en la fuente de M5GFX (Panel_ILI9342.hpp: memory_width=320,
     * memory_height=240), a diferencia del ILI9341 que nace 240x320 vertical
     * y si necesita el swap para quedar horizontal.
     *
     * Poner swap_xy(true) sobre un panel que YA es horizontal deja al
     * controlador direccionando 240 columnas mientras nosotros le mandamos
     * 320 pixeles por fila: cada fila se desborda 80 pixeles y la imagen se
     * va corriendo acumulativamente. Sintoma exacto visto en hardware real
     * el 19-20 ago: rellenos de color solido perfectos (correrse no se nota
     * si todo es del mismo color), texto y detalle fino corridos/fantasma, y
     * una franja inferior gris que era GRAM que nunca se llegaba a escribir.
     * Se confirmo por separado, volcando el buffer al log como ASCII, que
     * LVGL renderizaba el texto perfecto — el problema estaba solo aca. */
    /* Espejado en ambos ejes: sin esto la imagen sale rotada 180 grados
     * respecto de como se sostiene el Core2 (confirmado en hardware real
     * una vez corregido el swap_xy de arriba). */
    esp_lcd_panel_mirror(panel, true, true);
    esp_lcd_panel_disp_on_off(panel, true);

    ESP_LOGI(TAG, "core2_display_init OK (%dx%d)", CORE2_LCD_WIDTH, CORE2_LCD_HEIGHT);
    *out_panel = panel;
    *out_io = io_handle;
    return ESP_OK;
}
