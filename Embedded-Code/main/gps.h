/**
 * @file gps.h
 * @brief GPS module driver over UART (e.g. u-blox NEO-6M / NEO-M8N).
 *
 * Reads NMEA sentences from UART and exposes the latest raw line
 * through the sensor_driver_t interface (up to 82 bytes per NMEA sentence).
 */

#pragma once

#include "driver/uart.h"
#include "sensor_config.h"


/** Max NMEA sentence length (spec says 82 chars including CR LF) */
#define GPS_MAX_SENTENCE_LEN   128

/**
 * Initialise GPS UART. The `bus` parameter is ignored (GPS uses UART, not I2C).
 */
esp_err_t gps_init(uart_port_t port);

/**
 * Read the latest buffered NMEA line into out_data.
 * Returns ESP_OK if a line was available, ESP_ERR_NOT_FOUND if no new data.
 * `out_data` must be at least GPS_MAX_SENTENCE_LEN bytes.
 */
esp_err_t gps_read(uint8_t *out_data);

/**
 * Start the background UART receive task.
 * Call after gps_init().
 */
void gps_start_task(void);

extern const sensor_driver_t gps_driver;
