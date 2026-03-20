/**
 * @file bme680.c
 * @brief BME680 temperature / pressure / humidity driver (stub).
 * TODO: check this registers
 * Reads 8 raw bytes from register 0xF7:
 *   press_msb(1) press_lsb(1) press_xlsb(1)
 *   temp_msb(1)  temp_lsb(1)  temp_xlsb(1)
 *   hum_msb(1)   hum_lsb(1)
 *
 * TODO: (POST-PROCESSING) Add compensation formulas from the BME680 datasheet for
 *       calibrated temperature (°C), pressure (Pa), humidity (%RH).
 *       
 */

#include "bme680.h"
#include "i2c_bus.h"
#include "esp_log.h"

static const char *TAG = "bme680";

/* ── Register map ─────────────────────────────────────────── */
#define BME680_ADDR         0x76
#define REG_CHIP_ID         0xD0    /* Expected: 0x61 */
#define VAL_CHIP_ID         0x61
#define REG_CTRL_HUM        0xF2
#define REG_CTRL_MEAS       0xF4
#define REG_CONFIG          0xF5
#define REG_DATA_START      0xF7    /* 8 bytes: press(3) + temp(3) + hum(2) */

#define BME680_DATA_LEN     8       /* TODO: check data length for BME680 */

static i2c_master_dev_handle_t s_dev = NULL;
/* ── Public API ───────────────────────────────────────────── */

esp_err_t bme680_int(i2c_master_bus_handle_t bus) {
    esp_err_t ret = i2c_bind(bus, &s_dev, BME680_ADDR);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add BME680: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Ping device */
    uint8_t chip_id = 0;
    ret = i2c_bus_read_bytes(s_dev, REG_CHIP_ID, &chip_id, 1);

    /* Was connection possible? */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect with sensor: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Is it the right sensor? */
    if (chip_id != VAL_CHIP_ID) {
        ESP_LOGE(TAG, "Wrong Sensor: Expected 0x%02X, got 0x%02X", VAL_CHIP_ID, chip_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ret;
}

esp_err_t bme680_config(void)
{
    /* TODO: Check this configuration for BME680 */
    /* Humidity oversampling ×1 (must be set before ctrl_meas) */
    ret = i2c_bus_write_byte(s_dev, REG_CTRL_HUM, 0x01);
    if (ret != ESP_OK) return ret;

    /* Normal mode, temp oversample ×1, press oversample ×1 */
    ret = i2c_bus_write_byte(s_dev, REG_CTRL_MEAS, 0x27);
    if (ret != ESP_OK) return ret;

    /* Standby 0.5 ms, filter off */
    ret = i2c_bus_write_byte(s_dev, REG_CONFIG, 0x00);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "BME680 initialised (normal mode, 1× oversampling)");
    return ESP_OK;
}

esp_err_t bme680_read(uint8_t *out_data)
{
    return i2c_bus_read_bytes(s_dev, REG_DATA_START, out_data, BME680_DATA_LEN);
}

const sensor_driver_t bme680_driver = {
    .name     = "BME680",
    .data_len = BME680_DATA_LEN,
    .init     = bme680_init,
    .read     = bme680_read,
};
