/**
 * @file i2c_bus.h
 * @brief Shared I2C master bus — initialised once, used by all I2C sensors.
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/**
 * Initialise the I2C master bus.
 * Call once from app_main before any sensor init.
 */
esp_err_t i2c_bus_init(void);

/**
 * Get the bus handle so sensor drivers can add their devices.
 */
i2c_master_bus_handle_t i2c_bus_get_handle(void);

/**
 * Helper: write a single byte to a register on a device.
 */
esp_err_t i2c_bus_write_byte(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);

/**
 * Helper: read `len` bytes starting from `reg` on a device.
 */
esp_err_t i2c_bus_read_bytes(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len);
