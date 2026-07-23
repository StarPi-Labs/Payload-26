/**
 * @file bme680.h
 * @brief Bosch BME680 driver — RAW logging + flight-mode profiles.
 *
 * Sits on I2C1 (shared with the INA219). Auto-detects address 0x76/0x77 and
 * verifies chip id 0x61. Logs RAW register values; the Bosch compensation is
 * applied on the ground using the calibration frame (SBIT_CALIB):
 *   SBIT_BME680 = 8 raw bytes  (press[3] temp[3] hum[2], register order)
 *   SBIT_GAS    = 2 raw bytes  (gas_r_msb, gas_r_lsb) in POST/ARMED/COAST
 */

#ifndef _BME680_H_
#define _BME680_H_

#include "sensor_config.h"   /* esp_err_t, i2c_master_* types, sensor_driver_t */
#include <stdint.h>

/** Raw calibration blob length: coeff1(23 @0x8A) + coeff2(14 @0xE1) +
 *  extra 3 { res_heat_val(0x00), res_heat_range(0x02), range_sw_err(0x04) }. */
#define BME680_CALIB_BLOB_LEN   40

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

/** Raw calibration blob (BME680_CALIB_BLOB_LEN bytes). Valid after bme680_init. */
const uint8_t *bme680_get_calib_blob(uint16_t *len);

/** Background task: forced-mode reads, profile follows context.mode. */
void bme680_task(void *arg);

#endif /* _BME680_H_ */
