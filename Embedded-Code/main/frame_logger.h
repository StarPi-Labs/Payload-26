/**
 * @file frame_logger.h
 * @brief Binary frame logger for sensor data.
 *
 * Frame Structure:
 *   FRAME_SEPARATOR (4 bytes): 0xAAAAAAAA
 *   FRAME_HEADER:
 *     - Frame ID (2 bytes): incrementing counter (0x0000 - 0xFFFF)
 *     - Frame Info (2 bytes): sensor presence bitmap
 *   PAYLOAD (variable length):
 *     - Timestamp: millis() (4 bytes, uint32_t)
 *     - Accelerometer X, Y, Z (6 bytes, if present)
 *     - Gyroscope X, Y, Z (6 bytes, if present)
 *     - Temperature (4 bytes, if present)
 *     - Humidity (2 bytes, if present)
 *     - Pressure (4 bytes, if present)
 *     - GPS Data (up to 64 bytes, if present)
 *     - Air Quality Data (4 bytes, if present)
 *     - Power Monitor Data (8 bytes, if present) - INA219 extension
 *   CRC-16 CHECKSUM (2 bytes): CRC-CCITT (poly 0x1021)
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════
 *  Frame Constants
 * ═══════════════════════════════════════════════════════════ */

/** Frame separator marker */
#define FRAME_SEPARATOR         0xAAAAAAAA

/** Sensor presence bitmap bits */
#define SENSOR_BIT_ACCEL        (1 << 0)    /**< Accelerometer X, Y, Z (6 bytes) */
#define SENSOR_BIT_GYRO         (1 << 1)    /**< Gyroscope X, Y, Z (6 bytes) */
#define SENSOR_BIT_TEMP         (1 << 2)    /**< Temperature (4 bytes) */
#define SENSOR_BIT_HUMIDITY     (1 << 3)    /**< Humidity (2 bytes) */
#define SENSOR_BIT_PRESSURE     (1 << 4)    /**< Pressure (4 bytes) */
#define SENSOR_BIT_GPS          (1 << 5)    /**< GPS Data (~64 bytes) */
#define SENSOR_BIT_AIR_QUALITY  (1 << 6)    /**< Air Quality (4 bytes) */
#define SENSOR_BIT_POWER        (1 << 7)    /**< Power Monitor - INA219 (8 bytes) */

/*-- Sensor Presense Reduced bitmap  */
#define SBIT_MPU6050    (1 << 0)    // Covers Accelerometer and Gyros.
#define SBIT_BME680     (1 << 1)    // Covers Temp, Hum, and Pressure.
#define SBIT_MQ10       (1 << 2)    // GPS.
#define SBIT_INA219     (1 << 3)    // Power and Current.
#define SBIT_SYSSTATE   (1 << 4)    // Flight mode (1 byte), emitted on mode change.
#define SBIT_GAS        (1 << 5)    // BME680 raw gas registers (2 bytes: gas_r_msb, gas_r_lsb).
#define SBIT_CALIB      (1 << 6)    // Calibration frame: raw sensor constants for ground-side
                                    // conversion. Written at the head of every log session and
                                    // sent once over telemetry at power-up.
#define SBIT_RESERVED2  (1 << 7)    // Reserved

/*-- Universal Header */
typedef struct __attribute__((packed)) {
    uint8_t frame_separator[3]; // synchronization xAA, 0xAA, 0xAA
    uint8_t frame_info;         // sensor's name
    uint32_t timestamp_ms;      // 4 bytes: times in milisecods
} frame_header_t;

typedef struct __attribute__((packed)) {
    uint8_t AXH; uint8_t AXL;
    uint8_t AYH; uint8_t AYL;
    uint8_t AZH; uint8_t AZL;
    uint8_t GXH; uint8_t GXL;
    uint8_t GYH; uint8_t GYL;
    uint8_t GZH; uint8_t GZL;
} imu_payload_t;

/** Data sizes for each sensor field (in bytes) */
#define ACCEL_DATA_LEN          6
#define GYRO_DATA_LEN           6
#define TEMP_DATA_LEN           4
#define HUMIDITY_DATA_LEN       2
#define PRESSURE_DATA_LEN       4
#define GPS_DATA_LEN            64
#define AIR_QUALITY_DATA_LEN    4
#define POWER_DATA_LEN          8

/** Header sizes */
#define FRAME_SEPARATOR_LEN     4
#define FRAME_ID_LEN            2
#define FRAME_INFO_LEN          2
#define TIMESTAMP_LEN           4
#define CRC_LEN                 2

/** Maximum frame size (all sensors present) */
#define FRAME_MAX_SIZE          (FRAME_SEPARATOR_LEN + FRAME_ID_LEN + FRAME_INFO_LEN + \
                                 TIMESTAMP_LEN + ACCEL_DATA_LEN + GYRO_DATA_LEN + \
                                 TEMP_DATA_LEN + HUMIDITY_DATA_LEN + PRESSURE_DATA_LEN + \
                                 GPS_DATA_LEN + AIR_QUALITY_DATA_LEN + POWER_DATA_LEN + CRC_LEN)

/* ═══════════════════════════════════════════════════════════
 *  Frame Builder — call in order: begin, add_*, finish
 * ═══════════════════════════════════════════════════════════ */

/**
 * Frame builder context (stack-allocatable).
 */
typedef struct {
    uint8_t  buffer[FRAME_MAX_SIZE];
    size_t   offset;            /**< Current write position */
    size_t   payload_start;     /**< Where payload begins (after header) */
    uint16_t frame_id;          /**< Current frame ID */
    uint16_t sensor_bitmap;     /**< Which sensors are present */
} frame_builder_t;

esp_err_t logging_init(int *fd, char *filename);
void logging_task(void *arg);

/**
 * Initialize a frame builder and write separator + header placeholder.
 * Call this first for each new frame.
 *
 * @param fb        Frame builder context
 * @param frame_id  Frame sequence number
 */
void frame_begin(frame_builder_t *fb, uint16_t frame_id);

/**
 * Add timestamp to the frame.
 * Must be called right after frame_begin().
 *
 * @param fb        Frame builder context
 * @param timestamp Milliseconds since boot
 */
void frame_add_timestamp(frame_builder_t *fb, uint32_t timestamp);

/**
 * Add accelerometer data.
 * @param fb    Frame builder context
 * @param data  6 bytes: X(2) + Y(2) + Z(2) as int16_t big-endian
 */
void frame_add_accel(frame_builder_t *fb, const uint8_t *data);

/**
 * Add gyroscope data.
 * @param fb    Frame builder context
 * @param data  6 bytes: X(2) + Y(2) + Z(2) as int16_t big-endian
 */
void frame_add_gyro(frame_builder_t *fb, const uint8_t *data);

/**
 * Add temperature data.
 * @param fb    Frame builder context
 * @param data  4 bytes: temperature (int32_t or raw)
 */
void frame_add_temperature(frame_builder_t *fb, const uint8_t *data);

/**
 * Add humidity data.
 * @param fb    Frame builder context
 * @param data  2 bytes: humidity (uint16_t or raw)
 */
void frame_add_humidity(frame_builder_t *fb, const uint8_t *data);

/**
 * Add pressure data.
 * @param fb    Frame builder context
 * @param data  4 bytes: pressure (int32_t or raw)
 */
void frame_add_pressure(frame_builder_t *fb, const uint8_t *data);

/**
 * Add GPS data.
 * @param fb    Frame builder context
 * @param data  Up to GPS_DATA_LEN bytes of GPS data
 * @param len   Actual length of GPS data
 */
void frame_add_gps(frame_builder_t *fb, const uint8_t *data, size_t len);

/**
 * Add air quality data.
 * @param fb    Frame builder context
 * @param data  4 bytes: air quality reading
 */
void frame_add_air_quality(frame_builder_t *fb, const uint8_t *data);

/**
 * Add power monitor data (INA219).
 * @param fb    Frame builder context
 * @param data  8 bytes: shunt(2) + bus(2) + power(2) + current(2)
 */
void frame_add_power(frame_builder_t *fb, const uint8_t *data);

/**
 * Finalize frame: update sensor bitmap in header, compute and append CRC.
 * @param fb        Frame builder context
 * @return          Total frame size in bytes
 */
size_t frame_finish(frame_builder_t *fb);

/**
 * Get pointer to the completed frame data.
 * @param fb    Frame builder context
 * @return      Pointer to frame buffer
 */
const uint8_t *frame_get_data(const frame_builder_t *fb);

/* ═══════════════════════════════════════════════════════════
 *  CRC-16 CCITT
 * ═══════════════════════════════════════════════════════════ */

/**
 * Calculate CRC-16 CCITT (polynomial 0x1021, initial 0xFFFF).
 * @param data  Data buffer
 * @param len   Length in bytes
 * @return      16-bit CRC value
 */
void crc16_ccitt(uint16_t *crc, const uint8_t *data, size_t len);

/* ═══════════════════════════════════════════════════════════
 *  SD Card Binary Logger
 * ═══════════════════════════════════════════════════════════ */

/**
 * Mount SD card and open/create binary data file.
 * @return ESP_OK on success
 */
esp_err_t frame_logger_init(void);

/**
 * Write a complete frame to the SD card.
 * @param fb    Frame builder with finished frame
 * @return      ESP_OK on success
 */
esp_err_t frame_logger_write(const frame_builder_t *fb);

/**
 * Flush buffered data to SD card.
 */
void frame_logger_flush(void);

/**
 * Close file and unmount SD card.
 */
void frame_logger_deinit(void);

/**
 * Get number of frames written.
 */
uint32_t frame_logger_get_count(void);

/**
 * Get the open file handle for direct writes.
 * Returns NULL if SD is not mounted.
 */
FILE *frame_logger_get_file(void);


/**
 *-- TELEMETRY --
 */
void send_telemetry(uint8_t packet, void *data, uint16_t payload_size);

/**
 * -- LOGGER --
 */

typedef struct LoggerBuffer LoggerBuffer;
LoggerBuffer *logger_buff_init(void);
void write_to_ring_buffer(struct LoggerBuffer *buf, uint8_t type, void *payload, uint16_t payload_size);

/**
 * Build one complete frame (sync + type + timestamp + payload + CRC16) into dst.
 * dst must hold sizeof(frame_header_t) + payload_size + 2 bytes.
 * @return total frame size in bytes.
 */
uint16_t frame_build(uint8_t *dst, uint8_t type, const void *payload, uint16_t payload_size);

/**
 * Stash the calibration-frame payload. The SD logging task writes it (as an
 * SBIT_CALIB frame) at the head of every log session. Call before
 * logging_start_task, i.e. right after the sensors are initialised.
 */
void logging_set_calib(const void *payload, uint16_t len);

/** Get the stashed calibration payload. Returns its length (0 = none set). */
uint16_t logging_get_calib(const void **payload);

/** Calibration payload layout (version 1) — must match the ground parsers. */
struct __attribute__((packed)) calib_frame_v1 {
    uint8_t  version;                       /* = 1 */
    uint8_t  bme680[40];                    /* raw calib blob, see bme680.h */
    uint16_t mpu_accel_fs_g_armed;          /* accel full-scale outside BOOST [g]  (2)  */
    uint16_t mpu_accel_fs_g_boost;          /* accel full-scale in BOOST [g]       (16) */
    uint16_t mpu_gyro_fs_dps;               /* gyro full-scale [deg/s]             (500)*/
    uint16_t ina_shunt_mohm;                /* INA219 shunt resistance [milliohm]  (100)*/
};
