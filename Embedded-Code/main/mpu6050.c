/**
 * @file mpu6050.c
 * @brief MPU-6050 accelerometer + gyroscope driver.
 *
 * Reads 14 bytes per sample:
 *   accel_x(2) accel_y(2) accel_z(2)  temp(2)  gyro_x(2) gyro_y(2) gyro_z(2)
 *
 * All values are big-endian signed 16-bit.
 * Convert to real units with the full-scale sensitivity:
 *   ±2 g  → 16384 LSB/g      ±250 °/s → 131 LSB/(°/s)
 */

#include "mpu6050.h"
#include "i2c_bus.h"
#include "esp_log.h"

static const char *TAG = "mpu6050";

/* ── Register map ─────────────────────────────────────────── */
#define MPU6050_ADDR            0x68

#define REG_SMPLRT_DIV          0x19
#define REG_CONFIG              0x1A
#define REG_GYRO_CONFIG         0x1B
#define REG_ACCEL_CONFIG        0x1C
#define REG_ACCEL_XOUT_H        0x3B   /* Start of 14-byte burst read */
#define REG_PWR_MGMT_1          0x6B
#define REG_WHO_AM_I            0x75

#define MPU6050_DATA_LEN        14     /* accel(6) + temp(2) + gyro(6) */

static i2c_master_dev_handle_t s_dev = NULL;

/* ── Public API ───────────────────────────────────────────── */

esp_err_t mpu6050_init(i2c_master_bus_handle_t bus)
{
    /* Add device to bus */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = MPU6050_ADDR,
        .scl_speed_hz    = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add MPU6050 to I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Verify WHO_AM_I (expected 0x68) */
    uint8_t who = 0;
    ret = i2c_bus_read_bytes(s_dev, REG_WHO_AM_I, &who, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "WHO_AM_I = 0x%02X", who);

    /* Wake up (clear SLEEP bit) and use internal 8 MHz oscillator */
    ret = i2c_bus_write_byte(s_dev, REG_PWR_MGMT_1, 0x00);
    if (ret != ESP_OK) return ret;

    /* Sample rate divider: ODR = 1 kHz / (1 + div).  div=0 → 1 kHz */
    ret = i2c_bus_write_byte(s_dev, REG_SMPLRT_DIV, 0x00);
    if (ret != ESP_OK) return ret;

    /* DLPF bandwidth = 44 Hz (good noise / latency trade-off) */
    ret = i2c_bus_write_byte(s_dev, REG_CONFIG, 0x03);
    if (ret != ESP_OK) return ret;

    /* Gyro full-scale ±500 °/s */
    ret = i2c_bus_write_byte(s_dev, REG_GYRO_CONFIG, 0x08);
    if (ret != ESP_OK) return ret;

    /* Accel full-scale ±4 g */
    ret = i2c_bus_write_byte(s_dev, REG_ACCEL_CONFIG, 0x08);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "MPU6050 initialised  (±4 g, ±500 °/s, DLPF 44 Hz)");
    return ESP_OK;
}

esp_err_t mpu6050_read(uint8_t *out_data)
{
    return i2c_bus_read_bytes(s_dev, REG_ACCEL_XOUT_H, out_data, MPU6050_DATA_LEN);
}

/* ── Driver descriptor ────────────────────────────────────── */

const sensor_driver_t mpu6050_driver = {
    .name     = "MPU6050",
    .data_len = MPU6050_DATA_LEN,
    .init     = mpu6050_init,
    .read     = mpu6050_read,
};
