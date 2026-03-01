/**
 * @file mpu6050.h
 * @brief MPU-6050 accelerometer + gyroscope driver (I2C, address 0x68).
 */

#pragma once

#include "sensor_config.h"

/**
 * Initialise MPU6050: wake it up and set default full-scale ranges.
 * Adds the device to the shared I2C bus.
 */
esp_err_t mpu6050_init(i2c_master_bus_handle_t bus);

/**
 * Read 14 raw bytes: accel XYZ (6) + temp (2) + gyro XYZ (6).
 * @param out_data  buffer of at least 14 bytes
 */
esp_err_t mpu6050_read(uint8_t *out_data);

/** Pre-built sensor_driver_t you can register directly in main */
extern const sensor_driver_t mpu6050_driver;
