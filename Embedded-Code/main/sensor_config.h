/**
 * @file sensor_config.h
 * @brief Central configuration for all sensors and peripherals.
 *
 * Sensor enable/disable flags come from Kconfig (menuconfig).
 * Run `idf.py menuconfig` -> "Star-PI Payload Configuration" to toggle sensors.
 *
 * To add a new sensor:
 *   1. Add a Kconfig bool in Kconfig.projbuild
 *   2. Create sensor_name.h / sensor_name.c with init/read functions
 *   3. Register it in main.c inside the #if CONFIG_ENABLE_xxx block
 */

#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include "driver/i2c_master.h"

/* ──────────────────────────────────────────────────────────
 *  Pin Assignments  (override these in Kconfig if needed)
 * ────────────────────────────────────────────────────────── */

// I2C
#define PIN_I2C_SDA             CONFIG_I2C_MASTER_SDA
#define PIN_I2C_SCL             CONFIG_I2C_MASTER_SCL
#define I2C_MASTER_FREQ_HZ      CONFIG_I2C_MASTER_FREQUENCY

// GPS UART
#define GPS_UART_NUM            UART_NUM_2
#define PIN_GPS_TX              CONFIG_GPS_TXD_PIN
#define PIN_GPS_RX              CONFIG_GPS_RXD_PIN
#define GPS_BAUD_RATE           CONFIG_GPS_BAUD_RATE

// SD Card (SDMMC 1-bit mode)
#define PIN_SD_CLK              CONFIG_SD_CLK_PIN
#define PIN_SD_CMD              CONFIG_SD_CMD_PIN
#define PIN_SD_D0               CONFIG_SD_D0_PIN

/* ──────────────────────────────────────────────────────────
 *  Generic sensor interface
 * ────────────────────────────────────────────────────────── */

/** Maximum raw data bytes a single sensor can produce per read */
#define SENSOR_MAX_DATA_LEN     14

/** Opaque sensor descriptor — each driver fills one of these */
typedef struct {
    const char *name;                       /**< Human-readable name */
    uint8_t     data_len;                   /**< Bytes produced per read */
    esp_err_t (*init)(i2c_master_bus_handle_t bus);  /**< Init (may be NULL for UART sensors) */
    esp_err_t (*read)(uint8_t *out_data);   /**< Read raw data into out_data */
} sensor_driver_t;

/** Maximum number of sensor drivers that can be registered */
#define MAX_SENSORS  8

#endif // included via pragma once
