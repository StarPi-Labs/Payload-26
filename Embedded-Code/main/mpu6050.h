/**
 * @file mpu6050.h
 * @brief MPU-6050 accelerometer + gyroscope driver (I2C, address 0x68).
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sensor_config.h"


/**
 * Initialise MPU6050:
 * - Binds I2C
 * - Pings Device
 */
esp_err_t mpu6050_init(i2c_master_bus_handle_t bus);

/**
 * wake it up and set default full-scale ranges.
 */
esp_err_t mpu6050_config(void);
void mpu6050_start_isr(TaskHandle_t *task_handle);
void mpu6050_task(void *arg);

/**
 * Read 14 raw bytes: accel XYZ (6) + temp (2) + gyro XYZ (6).
 * @param out_data  buffer of at least 14 bytes
 */

/** Pre-built sensor_driver_t you can register directly in main */
extern const sensor_driver_t mpu6050_driver;
