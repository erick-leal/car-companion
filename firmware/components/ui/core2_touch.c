#include "core2_touch.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "core2_touch";

#define I2C_PORT      I2C_NUM_0   // mismo bus que el AXP192, ya instalado por core2_power_init
#define FT6336_ADDR   0x38

/* Mapa de registros del FT6336U (solo lo que usamos):
 *   0x02       cantidad de puntos tocados (bits 3:0)
 *   0x03 [3:0] X alto   | 0x04 X bajo
 *   0x05 [3:0] Y alto   | 0x06 Y bajo
 */
#define REG_TD_STATUS 0x02

esp_err_t core2_touch_init(void)
{
    /* Chequeo de presencia: leer el registro de estado. Si el chip no
     * responde, mejor enterarnos ahora por log que quedarnos sin entender
     * por que el tactil "no hace nada". */
    uint8_t reg = REG_TD_STATUS;
    uint8_t val = 0;
    esp_err_t err = i2c_master_write_read_device(I2C_PORT, FT6336_ADDR, &reg, 1,
                                                  &val, 1, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "el FT6336U no responde en I2C (0x%02X): %s — el tactil no va a andar",
                 FT6336_ADDR, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "core2_touch_init OK (FT6336U presente)");
    return ESP_OK;
}

bool core2_touch_read(uint16_t *out_x, uint16_t *out_y)
{
    uint8_t reg = REG_TD_STATUS;
    uint8_t buf[5] = {0}; // 0x02..0x06

    if (i2c_master_write_read_device(I2C_PORT, FT6336_ADDR, &reg, 1,
                                      buf, sizeof(buf), pdMS_TO_TICKS(20)) != ESP_OK) {
        return false;
    }

    uint8_t points = buf[0] & 0x0F;
    if (points == 0 || points > 2) {
        return false;
    }

    *out_x = (uint16_t)((buf[1] & 0x0F) << 8 | buf[2]);
    *out_y = (uint16_t)((buf[3] & 0x0F) << 8 | buf[4]);
    return true;
}
