/**
 * @file i2c_bus.h
 * @brief Shared I2C master bus — initialised once, used by all I2C sensors.
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/**
 * Initialise the I2C master buses.
 * Call once from app_main before any sensor init.
 */
esp_err_t sys_i2c0_init(gpio_num_t sda_pin, gpio_num_t scl_pin);
esp_err_t sys_i2c1_init(gpio_num_t sda_pin, gpio_num_t scl_pin);

/**
 * Binds bus and device
 * Call once from the device driver
 */
esp_err_t i2c_bind(
    i2c_master_bus_handle_t bus, 
    i2c_master_dev_handle_t *dev,
    uint16_t device_addr);

/**
 * Get the bus handles so sensor drivers can add their devices.
 */
i2c_master_bus_handle_t i2c_bus0_get_handle(void);
i2c_master_bus_handle_t i2c_bus1_get_handle(void);

/**
 * Helper: write a single byte to a register on a device.
 */
esp_err_t i2c_bus_write_byte(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);

/**
 * Helper: read `len` bytes starting from `reg` on a device.
 */
esp_err_t i2c_bus_read_bytes(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len);
