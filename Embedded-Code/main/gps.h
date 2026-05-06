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
#include "systemp2i.h"

/** Max NMEA sentence length (spec says 82 chars including CR LF) */
#define GPS_MAX_SENTENCE_LEN   128

/*--- GPS Info required for the payload */
#define GPS_SPEED_STR_SZ    10
#define GPS_COURSE_STR_SZ   10
#define GPS_TIME_STR_SZ     10
#define GPS_LAT_STR_SZ      11
#define GPS_LON_STR_SZ      12
#define GPS_SATCOUNT_STR_SZ 4
#define GPS_ALT_STR_SZ      8
/*NOTE: 
 * The sum of these guys go into the frameparser.py under MQ10 data size */

struct __attribute__((packed)) GPSInfo {
    char status;                    // 'A' or 'V'
    char speed[GPS_SPEED_STR_SZ];   // Unknown length
    char course[GPS_COURSE_STR_SZ]; // Unknown length
    
    char time[GPS_TIME_STR_SZ];     // 'HHMMSS.SS\0'
    char lat[GPS_LAT_STR_SZ];       // 'ddmm.mmmmm\0'
    char lat_orientation;           // 'N' or 'S'
    char lon[GPS_LON_STR_SZ];       // 'dddmm.mmmmm\0'
    char lon_orientation;           // 'E' or 'W'
    char sat_count[GPS_SATCOUNT_STR_SZ]; // Often something ranging between 0 and 70-ish, 
                                    // but let's assume it could be more than that
    char alt[GPS_ALT_STR_SZ];       // Altitude 
    uint8_t available;              // When everything is parsed this is raised.
};

/**
 * Initialise GPS UART. The `bus` parameter is ignored (GPS uses UART, not I2C).
 */
esp_err_t gps_probe(uart_port_t port);
void gps_upgrade_baud_115200(uart_port_t port);
void gps_disable_non_essential_NMEA(uart_port_t port);
void gps_rx_task(void *arg);

extern const sensor_driver_t gps_driver;
