/**
 * @file i2c_bus.c
 * @brief Shared I2C master bus implementation.
 */

#include "i2c_bus.h"
#include "sensor_config.h"
#include "esp_log.h"

static const char *TAG = "i2c_bus";

#define I2C_TIMEOUT_MS  10

static i2c_master_bus_handle_t s_bus_handle = NULL;

esp_err_t i2c_bus_init(void)
{
    if (s_bus_handle != NULL) {
        ESP_LOGW(TAG, "I2C bus already initialised");
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = PIN_I2C_SDA,
        .scl_io_num        = PIN_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "I2C bus ready  SDA=%d  SCL=%d  %d Hz",
                 PIN_I2C_SDA, PIN_I2C_SCL, I2C_MASTER_FREQ_HZ);
    } else {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    return s_bus_handle;
}

esp_err_t i2c_bus_write_byte(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
}

esp_err_t i2c_bus_read_bytes(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
}
