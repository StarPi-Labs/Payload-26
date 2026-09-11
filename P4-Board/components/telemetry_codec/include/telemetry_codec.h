/**
 * @file telemetry_codec.h
 * @brief Decoder for the Pi-LOG binary telemetry stream.
 *
 * C port of SD-Parser/frameparser.py. Feed it bytes from any transport - ESP-NOW,
 * UDP, UART, a file - and it maintains a forward-filled snapshot of every sensor,
 * the same merged view SD-Parser/json2telemetry.py builds on the ground.
 *
 * Wire format (little-endian throughout):
 *
 *   AA AA AA | type | timestamp_ms (u32) | payload | crc16 (u16)
 *   \___ 3 ___/  1   \_______ 4 ________/           \___ 2 ___/
 *
 * CRC-16/CCITT, init 0xFFFF, poly 0x1021, computed over everything before it.
 *
 * The stream is resynchronising by design: a corrupt or truncated frame is
 * dropped and the decoder hunts for the next sync word. That is what lets the
 * transports drop whole chunks under load without corrupting the output.
 *
 * Scaling constants (accelerometer full-scale, gyro full-scale, INA219 shunt,
 * BME680 calibration) arrive in a CALIB frame that the payload emits at the
 * head of every session and repeats periodically. Until one is seen, sensible
 * defaults are used and `have_calib` stays false - readings before that point
 * are approximate.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Flight modes - must match the firmware enum in systemp2i.h. */
typedef enum {
    TELEM_MODE_INIT = 0,
    TELEM_MODE_POST,
    TELEM_MODE_SENSOR_CHECK,
    TELEM_MODE_ARMED,
    TELEM_MODE_BOOST,
    TELEM_MODE_COAST,
    TELEM_MODE_COUNT
} telem_mode_t;

/** Short label for a mode value ("ARMED", "BOOST", ...). Safe for any input. */
const char *telemetry_mode_name(uint8_t mode);

/**
 * Forward-filled snapshot of every sensor.
 *
 * Each field holds the most recent value seen. Frames carry one sensor each,
 * so at any instant this is a merge across several frames - exactly what the
 * ground-side CSV pipeline produces.
 */
typedef struct {
    uint32_t timestamp_ms;      /**< payload uptime of the newest frame */

    /* MPU6500 (labelled MPU6050 on the board) */
    float accel_x, accel_y, accel_z;   /**< g */
    float accel_mag;                   /**< g, vector magnitude */
    float roll, pitch, yaw;            /**< deg/s - gyro RATES, not angles */

    /* BME680 */
    float temperature;          /**< degC */
    float pressure;             /**< kPa */
    float humidity;             /**< %RH */
    float gas_resistance;       /**< ohm */

    /* Derived from pressure, referenced to the first sample seen */
    float altitude;             /**< m above the pad */
    float vertical_velocity;    /**< m/s, first difference of altitude */

    /* GPS */
    double gps_lat, gps_lon;    /**< degrees, signed */
    float  gps_alt;             /**< m MSL */
    float  horizontal_velocity; /**< m/s */
    uint8_t sat_count;
    bool    gps_fix;

    /* INA219 */
    float bus_voltage;          /**< V */
    float current;              /**< mA */
    float power;                /**< W */

    /* Flight state */
    uint8_t mode;

    /* Link health */
    uint32_t frames_ok;
    uint32_t frames_crc_fail;
    bool     have_calib;
} telem_state_t;

/** Decoder context. Zero-initialise, then call telemetry_codec_init(). */
typedef struct {
    /* Partial frame carried between feeds. Sized for the largest frame
     * (GPS, 68 B payload) with generous slack for resync hunting. */
    uint8_t  buf[512];
    size_t   used;

    /* Scaling, updated by CALIB frames. */
    float    accel_sens_low;    /**< LSB per g, non-BOOST range */
    float    accel_sens_high;   /**< LSB per g, BOOST range */
    float    gyro_sens;         /**< LSB per deg/s */
    float    ina_shunt_ohm;

    /* BME680 calibration coefficients, parsed from the CALIB blob. */
    struct {
        uint16_t t1; int16_t t2; int8_t t3;
        uint16_t p1; int16_t p2; int8_t p3; int16_t p4, p5;
        int8_t p6, p7; int16_t p8, p9; uint8_t p10;
        uint16_t h1, h2; int8_t h3, h4, h5; uint8_t h6; int8_t h7;
        uint8_t  res_heat_range; int8_t res_heat_val; int8_t range_sw_err;
        float    t_fine;
    } bme;

    /* Altitude reference and velocity differentiation state. */
    float    ground_pressure;   /**< kPa, first pressure sample seen */
    float    last_alt;
    uint32_t last_alt_ms;
    bool     have_last_alt;

    telem_state_t state;
} telemetry_codec_t;

/** Reset a decoder to defaults. Call once before feeding anything. */
void telemetry_codec_init(telemetry_codec_t *c);

/**
 * Feed received bytes. Decodes every complete frame present and updates
 * `c->state`. Any trailing partial frame is retained for the next call, so
 * callers may pass arbitrary chunk boundaries.
 */
void telemetry_codec_feed(telemetry_codec_t *c, const uint8_t *data, size_t len);

/** Convenience accessor. */
static inline const telem_state_t *telemetry_codec_state(const telemetry_codec_t *c)
{
    return &c->state;
}
