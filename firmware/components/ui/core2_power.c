#include "core2_power.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "core2_power";

#define I2C_PORT      I2C_NUM_0
#define I2C_SDA_GPIO  21
#define I2C_SCL_GPIO  22
#define I2C_FREQ_HZ   400000

#define AXP192_ADDR   0x34

static esp_err_t axp192_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(I2C_PORT, AXP192_ADDR, buf, sizeof(buf),
                                       pdMS_TO_TICKS(100));
}

static esp_err_t axp192_read(uint8_t reg, uint8_t *out_val)
{
    return i2c_master_write_read_device(I2C_PORT, AXP192_ADDR, &reg, 1, out_val, 1,
                                         pdMS_TO_TICKS(100));
}

/* read-modify-write: la mayoria de los registros del AXP192 comparten bits
 * con otras cosas (GPIOs, otros rieles) — pisar el byte entero (como hacia
 * la version anterior con 0x12=0xFF) prende cosas que no deberian, y en la
 * practica ESO fue lo que encendio el vibrador (LDO3, bit 3 del 0x12) sin
 * querer el 19 ago. Nunca mas escribir un registro de este chip entero sin
 * antes leerlo. */
static esp_err_t axp192_rmw(uint8_t reg, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t cur = 0;
    esp_err_t err = axp192_read(reg, &cur);
    if (err != ESP_OK) return err;
    return axp192_write(reg, (cur & ~clear_mask) | set_mask);
}

esp_err_t core2_power_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_PORT, &conf);
    if (err != ESP_OK) return err;
    err = i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
    if (err != ESP_OK) return err;

    /* Secuencia calcada de AXP192::begin() en
     * https://github.com/m5stack/M5Core2/blob/master/src/AXP192.cpp
     * (la libreria oficial de M5Stack para el Core2) — no de memoria esta
     * vez, se confirmo registro por registro contra esa fuente el 19 ago
     * despues de que la version anterior (adivinada) prendiera el
     * vibrador sin querer. */
    esp_err_t rc = ESP_OK;

    rc |= axp192_rmw(0x30, 0xFB, 0x02);              // vbus limit off
    rc |= axp192_rmw(0x92, 0x07, 0x00);               // GPIO1 open-drain output
    rc |= axp192_rmw(0x93, 0x07, 0x00);               // GPIO2 open-drain output
    rc |= axp192_rmw(0x35, 0xE3, 0xA2);               // RTC: carga de bateria de respaldo habilitada

    rc |= axp192_rmw(0x26, 0x7F, 0x6A);               // DCDC1 (alimentacion del ESP32) = 3.35V (preserva el bit 7 existente)
    rc |= axp192_rmw(0x27, 0x7F, 0x54);               // DCDC3 (LCD backlight) = 2.80V (preserva el bit 7 existente)
    rc |= axp192_write(0x28, 0xF2);                    // LDO2 (logica LCD/SD) = 3.3V, LDO3 (vibrador) preset 2.0V pero SIN habilitar

    /* Habilitar LDO2 (logica LCD) y DCDC3 (backlight LCD), y FORZAR apagado
     * el LDO3 (vibrador, bit 3) de forma explicita — no alcanza con "no
     * tocarlo": el AXP192 tiene un dominio de respaldo (RTC/LDO1) que puede
     * sobrevivir al apagado con el boton, asi que el bit que quedo prendido
     * por el bug original puede persistir entre reinicios. Confirmado en
     * hardware real el 19 ago: con esta linea como estaba antes (sin el
     * clear explicito de bit 3) el vibrador seguia andando, mas debil,
     * despues de "arreglar" el resto. */
    rc |= axp192_rmw(0x12, (1 << 3), (1 << 2) | (1 << 1));

    rc |= axp192_rmw(0x33, 0x0F, 0x00);               // corriente de carga de bateria: 100mA
    rc |= axp192_rmw(0x95, 0x8D, 0x84);               // habilitar GPIO4 (reset de LCD/touch) como salida
    rc |= axp192_rmw(0x94, 0x02, 0x00);               // LED indicador de power encendido (bit invertido: 0=on)
    rc |= axp192_write(0x36, 0x4C);                    // configuracion del boton de power
    rc |= axp192_write(0x82, 0xFF);                    // habilitar todos los ADC (bateria/temp/etc)

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "fallo escribiendo registros de power-on del AXP192");
        return ESP_FAIL;
    }

    /* Pulso de reset a la pantalla: reg 0x96 bit 0x02 (GPIO4 output level).
     * OJO: NO es el reg 0x94 (ese es el LED indicador de power, otra cosa —
     * confundirlos fue el segundo bug de la version anterior). */
    axp192_rmw(0x96, 0x02, 0x00); // reset activo (en bajo)
    vTaskDelay(pdMS_TO_TICKS(100));
    axp192_rmw(0x96, 0x00, 0x02); // libera el reset
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "core2_power_init OK (AXP192 configurado, LCD alimentado, vibrador apagado)");
    return ESP_OK;
}

int core2_power_get_battery_pct(void)
{
    /* ADC de bateria: 12 bits repartidos en dos registros consecutivos
     * (0x78 = byte alto, 0x79 = byte bajo), 1.1mV por cuenta. Formula de
     * porcentaje calcada de AXP192::GetBatteryLevel() de M5Stack — no es
     * una curva de descarga real de LiPo, es la aproximacion lineal que usa
     * la libreria oficial. */
    uint8_t hi = 0, lo = 0;
    if (axp192_read(0x78, &hi) != ESP_OK || axp192_read(0x79, &lo) != ESP_OK) {
        return -1;
    }
    uint16_t adc = ((uint16_t)hi << 4) + lo;
    float volts = adc * (1.1f / 1000.0f);

    if (volts < 3.248088f) return 0;
    float pct = (volts - 3.120712f) * 100.0f;
    if (pct > 100.0f) pct = 100.0f;
    if (pct < 0.0f) pct = 0.0f;
    return (int)pct;
}
