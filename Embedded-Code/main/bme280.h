/**
 * @file bme280.h
 * @brief BME280 temperature / pressure / humidity driver stub (I2C, address 0x76).
 *
 * TODO: Implement full compensation algorithm for real physical values.
 *       For now this reads 8 raw bytes (pressure 3 + temperature 3 + humidity 2).
 */

#pragma once

#include "sensor_config.h"

esp_err_t bme280_init(i2c_master_bus_handle_t bus);
esp_err_t bme280_read(uint8_t *out_data);

extern const sensor_driver_t bme280_driver;
