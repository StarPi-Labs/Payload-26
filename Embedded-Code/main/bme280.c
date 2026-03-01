/**
 * @file bme280.c
 * @brief BME280 temperature / pressure / humidity driver (stub).
 *
 * Reads 8 raw bytes from register 0xF7:
 *   press_msb(1) press_lsb(1) press_xlsb(1)
 *   temp_msb(1)  temp_lsb(1)  temp_xlsb(1)
 *   hum_msb(1)   hum_lsb(1)
 *
 * TODO: Add compensation formulas from the BME280 datasheet for
 *       calibrated temperature (°C), pressure (Pa), humidity (%RH).
 */

#include "bme280.h"
#include "i2c_bus.h"
#include "esp_log.h"

static const char *TAG = "bme280";

#define BME280_ADDR         0x76
#define REG_CHIP_ID         0xD0    /* Expected: 0x60 */
#define REG_CTRL_HUM        0xF2
#define REG_CTRL_MEAS       0xF4
#define REG_CONFIG           0xF5
#define REG_DATA_START      0xF7    /* 8 bytes: press(3) + temp(3) + hum(2) */

#define BME280_DATA_LEN     8

static i2c_master_dev_handle_t s_dev = NULL;

esp_err_t bme280_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BME280_ADDR,
        .scl_speed_hz    = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add BME280: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Verify chip ID */
    uint8_t id = 0;
    ret = i2c_bus_read_bytes(s_dev, REG_CHIP_ID, &id, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Chip ID read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Chip ID = 0x%02X (expect 0x60 for BME280)", id);

    /* Humidity oversampling ×1 (must be set before ctrl_meas) */
    ret = i2c_bus_write_byte(s_dev, REG_CTRL_HUM, 0x01);
    if (ret != ESP_OK) return ret;

    /* Normal mode, temp oversample ×1, press oversample ×1 */
    ret = i2c_bus_write_byte(s_dev, REG_CTRL_MEAS, 0x27);
    if (ret != ESP_OK) return ret;

    /* Standby 0.5 ms, filter off */
    ret = i2c_bus_write_byte(s_dev, REG_CONFIG, 0x00);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "BME280 initialised (normal mode, 1× oversampling)");
    return ESP_OK;
}

esp_err_t bme280_read(uint8_t *out_data)
{
    return i2c_bus_read_bytes(s_dev, REG_DATA_START, out_data, BME280_DATA_LEN);
}

const sensor_driver_t bme280_driver = {
    .name     = "BME280",
    .data_len = BME280_DATA_LEN,
    .init     = bme280_init,
    .read     = bme280_read,
};
