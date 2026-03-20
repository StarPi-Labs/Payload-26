/**
 * @file ina219.h
 * @brief INA219 current / voltage / power monitor driver (I2C, address 0x40).
 *
 * The INA219 measures:
 *   - Bus voltage (load side, 0–26V or 0–32V)
 *   - Shunt voltage (across current sense resistor)
 *   - Current (computed from shunt voltage and calibration)
 *   - Power (computed from bus voltage and current)
 *
 * Raw data: 8 bytes = shunt(2) + bus(2) + power(2) + current(2)
 */

#pragma once

#include "sensor_config.h"

/**
 * Initialize INA219 on the shared I2C bus.
 * Configures 32V bus range, ±320mV shunt, 12-bit resolution.
 */
esp_err_t ina219_init(i2c_master_bus_handle_t bus);
esp_err_t ina219_config(void);

/**
 * Read raw data: shunt voltage, bus voltage, power, current (8 bytes).
 */
esp_err_t ina219_read(uint8_t *out_data);

/**
 * Get bus voltage in millivolts (from last read).
 */
int32_t ina219_get_bus_voltage_mv(const uint8_t *data);

/**
 * Get shunt voltage in microvolts (from last read).
 */
int32_t ina219_get_shunt_voltage_uv(const uint8_t *data);

/**
 * Get current in microamps (from last read, requires calibration).
 */
int32_t ina219_get_current_ua(const uint8_t *data);

/**
 * Get power in microwatts (from last read, requires calibration).
 */
int32_t ina219_get_power_uw(const uint8_t *data);

extern const sensor_driver_t ina219_driver;
