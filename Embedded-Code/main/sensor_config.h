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
 *  The advatange of these is that the pins can be modified
 *  here without the need of modifying the Kconfig for fast 
 *  debuggin or test, it also allows easy visibility in the 
 *  mapping without hardcoding the drivers to specific pins
 * ────────────────────────────────────────────────────────── */
/*-- MPU6050 --*/
#define I2C_MASTER_CLK          CONFIG_I2C_MASTER_FREQUENCY
#define MPU6050_I2C             I2C_NUM_0
#define MPU6050_SDA             CONFIG_I2C0_MASTER_SDA
#define MPU6050_SCL             CONFIG_I2C0_MASTER_SCL
#define MPU6050_INT_PIN         34
#define MPU6050_CORE            1
#define MPU6050_PRIORITY        5                       // Highest Priority
#define MPU6050_HM_SKIP_SAMPLES 1000

/*-- BME680/INA219 --*/
#define BME_INA_I2C             I2C_NUM_1
#define BME_INA_SDA             CONFIG_I2C1_MASTER_SDA
#define BME_INA_SCL             CONFIG_I2C1_MASTER_SCL
#define INA219_CORE             1
#define BME680_CORE             1
#define BME680_PRIORITY         4                       // Medium Priority
#define INA219_PRIORITY         3                       // Lowest Priority
#define INA219_PERIOD_1ms       1                       // 1000Hz, max: 1.8kHz, period 532us
#define INA219_PERIOD_2ms       2                       // 500Hz    
#define INA219_PERIOD_10ms      10                      // 100Hz
#define INA219_PERIOD_100ms     100                     // 10Hz     (default and recommeded)
#define INA219_PERIOD_1s        1000                    // 1Hz
#define INA219_PERIOD_default   INA219_PERIOD_100ms
#define INA219_HM_SKIP_SAMPLES  10                      // TODO: right now this 
                                                        // can be sampled @ 1.8kHz
                                                        // however, for this application
                                                        // Sampled is capped to 10Hz

/*-- GPS UART --*/
#define GPS_UART                UART_NUM_2
#define GPS_UART_BAUD115200     115200
#define GPS_TX                  CONFIG_GPS_TXD_PIN
#define GPS_RX                  CONFIG_GPS_RXD_PIN
#define GPS_BAUD                CONFIG_GPS_BAUD_RATE
#define GPS_RX_BUF_SZ           1024
#define GPS_CORE                0
#define GPS_PRIORITY            1                       // Low priority, it is slow
#define GPS_HM_SKIP_SAMPLES     2                       // Skip samples while on Health Monitor

/*-- HEALTH_MONITOR UART --*/
// NOTE: Deprecated
// TODO: Remove it
// - Debugging  -> USB-Serial
// - Deployment -> LoRA
#define HEALTH_MONITOR_UART     UART_NUM_0              // Debugging purposes
#define HEALTH_MONITOR_CORE     1
#define HEALTH_MONITOR_BAUD     105200

// SD Card (SDMMC 1-bit mode)
#define SD_PORT                 SPI2_HOST
#define PIN_SD_CLK              CONFIG_SD_CLK_PIN
#define PIN_SD_CMD              CONFIG_SD_CMD_PIN
#define PIN_SD_D0               CONFIG_SD_D0_PIN
#define SDSPI_MOSI              CONFIG_SD_SPI_MOSI_PIN
#define SDSPI_MISO              CONFIG_SD_SPI_MISO_PIN
#define SDSPI_CLK               CONFIG_SD_SPI_SCLK_PIN
#define SDSPI_CS                CONFIG_SD_SPI_CS_PIN
#define FLIGHT_LOG_FILE_PATH    "/sd/pstarpi.bin"          // Name should be short
#define SD_INIT_SECTOR          4059008
#define SD_LAST_SECTOR          4169599
#define SD_SECTOR_COUNT         110592
#define SD_INIT_SECTOR_CONTENT  "ROCKET_START_2026"
#define SD_INIT_SECTOR_CONTENT_SZ 17
#define LOGGING_PRIORITY        1
#define LOGGING_CORE            0

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

/* ──────────────────────────────────────────────────────────
 *  Sensor specific
 * ────────────────────────────────────────────────────────── */



