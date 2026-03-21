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

static const char *TAG = "MPU6050";

/* ── Register map ─────────────────────────────────────────── */
#define MPU6050_ADDR            0x68

#define REG_SMPLRT_DIV          0x19
#define REG_CONFIG              0x1A
#define REG_GYRO_CONFIG         0x1B
#define REG_ACCEL_CONFIG        0x1C
#define REG_ACCEL_XOUT_H        0x3B   /* Start of 14-byte burst read */
#define REG_PWR_MGMT_1          0x6B
#define REG_WHO_AM_I            0x75
#define VAL_WHO_AM_I            0x68

#define MPU6050_DATA_LEN        14     /* accel(6) + temp(2) + gyro(6) */

static i2c_master_dev_handle_t s_dev = NULL;

/* ── Public API ───────────────────────────────────────────── */

esp_err_t mpu6050_init(i2c_master_bus_handle_t bus) {
    /* Bind Device to the i2c bus */
    esp_err_t ret = i2c_bind(bus, &s_dev, MPU6050_ADDR);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fail to bind MPU6050 to I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Ping device */
    uint8_t sensor_id = 0;
    ret = i2c_bus_read_bytes(s_dev, REG_WHO_AM_I, &sensor_id, 1);

    /* Was connection possible? */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect with sensor: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Is it the right sensor? */
    if (sensor_id != VAL_WHO_AM_I) {
        ESP_LOGE(TAG, "Wrong Sensor: Expected 0x%02X, got 0x%02X", VAL_WHO_AM_I, sensor_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ret;
}

esp_err_t mpu6050_config(void)
{
    /* Wake up (clear SLEEP bit) and use internal 8 MHz oscillator */
    esp_err_t ret = i2c_bus_write_byte(s_dev, REG_PWR_MGMT_1, 0x00);
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
