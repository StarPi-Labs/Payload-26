/**
 * @file bme680.h
 * @brief Bosch BME680 driver — on-device compensation + flight-mode profiles.
 *
 * Sits on I2C1 (shared with the INA219). Auto-detects address 0x76/0x77 and
 * verifies chip id 0x61. Applies the Bosch compensation on-device and logs
 * SBIT_BME680 as 3 floats {temperature °C, pressure Pa, humidity %RH}; the gas
 * resistance is logged separately as SBIT_GAS (1 float, ohm) in POST/ARMED/COAST.
 */

#ifndef _BME680_H_
#define _BME680_H_

#include "sensor_config.h"   /* esp_err_t, i2c_master_* types, sensor_driver_t */
#include <stdint.h>

/**
 * Raw field-0 register read (10 bytes, internal). Compensated on-device before
 * logging. Layout mirrors the BME680 field-0 data registers:
 *   press[3] = 0x1F..0x21 (press_msb, press_lsb, press_xlsb[7:4])
 *   temp[3]  = 0x22..0x24 (temp_msb,  temp_lsb,  temp_xlsb[7:4])
 *   hum[2]   = 0x25..0x26 (hum_msb,   hum_lsb)
 *   gas[2]   = 0x2A..0x2B (gas_r_msb, [gas_r_lsb<7:6>|gas_valid<5>|heat_stab<4>|gas_range<3:0>])
 *              -> both bytes are 0 when gas was not measured this cycle.
 */
struct __attribute__((packed)) BME680Raw {
    uint8_t press[3];
    uint8_t temp[3];
    uint8_t hum[2];
    uint8_t gas[2];
};

/** Bind on the given bus, auto-detect 0x76/0x77, verify id, read+dump calib. */
esp_err_t bme680_init(i2c_master_bus_handle_t bus);

/** Background task: forced-mode reads, profile follows context.mode. */
void bme680_task(void *arg);

#endif /* _BME680_H_ */
