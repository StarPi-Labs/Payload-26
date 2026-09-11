/**
 * @file telemetry_codec.c
 * @brief Pi-LOG telemetry decoder - implementation.
 *
 * Ported from SD-Parser/frameparser.py. The BME680 compensation is lifted
 * verbatim from Embedded-Code/main/bme680.c (Bosch float algorithms) so the
 * ground-side numbers match the payload's own, rather than being a second
 * independent implementation that could drift.
 */

#include "telemetry_codec.h"

#include <math.h>
#include <stdlib.h>    /* atof/atoi for the ASCII NMEA fields */
#include <string.h>

/* ── Protocol ─────────────────────────────────────────────────────────── */

#define HEADER_SIZE   8      /* AA AA AA | type | u32 timestamp */
#define FOOTER_SIZE   2      /* CRC-16 */

#define PKT_MPU6050   0x01
#define PKT_BME680    0x02
#define PKT_GPS       0x04
#define PKT_INA219    0x08
#define PKT_SYSSTATE  0x10
#define PKT_GAS       0x20
#define PKT_CALIB     0x40

/* Payload sizes, matching PACKET_DEFS in frameparser.py exactly.
 * The GPS entry is 68 and NOT 69: struct GPSInfo is packed and the firmware
 * deliberately sends sizeof(struct GPSInfo) - 1, excluding a bookkeeping-only
 * trailing field. Getting this wrong fails the CRC on every GPS frame. */
static int payload_size_for(uint8_t type)
{
    switch (type) {
    case PKT_MPU6050:  return 14;   /* 7 x int16 */
    case PKT_BME680:   return 8;    /* raw press[3] temp[3] hum[2] */
    case PKT_GPS:      return 68;
    case PKT_INA219:   return 4;    /* 2 x int16 */
    case PKT_SYSSTATE: return 1;
    case PKT_GAS:      return 2;
    case PKT_CALIB:    return 49;
    default:           return -1;
    }
}

/* INA219 fixed constants (see frameparser.py). */
#define INA219_MAX_VOLT  16.0f
#define INA219_ADC_BITS  4096.0f

/* ── CRC-16/CCITT ─────────────────────────────────────────────────────── */

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* ── Little-endian readers ────────────────────────────────────────────── */

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int16_t  rd_i16(const uint8_t *p) { return (int16_t)rd_u16(p); }
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── BME680 compensation (lifted from Embedded-Code/main/bme680.c) ────── */

/* Blob layout: coeff1(23 @0x8A) + coeff2(14 @0xE1) + 3 extra regs = 40 bytes. */
#define CALIB1_LEN 23
#define CALIB2_LEN 14
#define CB1(blob, reg)  ((blob)[(reg) - 0x8A])
#define CB2(blob, reg)  ((blob)[CALIB1_LEN + ((reg) - 0xE1)])

static void bme_parse_calib(telemetry_codec_t *c, const uint8_t *b)
{
    c->bme.t1 = (uint16_t)((CB2(b, 0xEA) << 8) | CB2(b, 0xE9));
    c->bme.t2 = (int16_t)((CB1(b, 0x8B) << 8) | CB1(b, 0x8A));
    c->bme.t3 = (int8_t)CB1(b, 0x8C);

    c->bme.p1  = (uint16_t)((CB1(b, 0x8F) << 8) | CB1(b, 0x8E));
    c->bme.p2  = (int16_t)((CB1(b, 0x91) << 8) | CB1(b, 0x90));
    c->bme.p3  = (int8_t)CB1(b, 0x92);
    c->bme.p4  = (int16_t)((CB1(b, 0x95) << 8) | CB1(b, 0x94));
    c->bme.p5  = (int16_t)((CB1(b, 0x97) << 8) | CB1(b, 0x96));
    c->bme.p6  = (int8_t)CB1(b, 0x99);
    c->bme.p7  = (int8_t)CB1(b, 0x98);
    c->bme.p8  = (int16_t)((CB1(b, 0x9D) << 8) | CB1(b, 0x9C));
    c->bme.p9  = (int16_t)((CB1(b, 0x9F) << 8) | CB1(b, 0x9E));
    c->bme.p10 = (uint8_t)CB1(b, 0xA0);

    c->bme.h1 = (uint16_t)((CB2(b, 0xE3) << 4) | (CB2(b, 0xE2) & 0x0F));
    c->bme.h2 = (uint16_t)((CB2(b, 0xE1) << 4) | (CB2(b, 0xE2) >> 4));
    c->bme.h3 = (int8_t)CB2(b, 0xE4);
    c->bme.h4 = (int8_t)CB2(b, 0xE5);
    c->bme.h5 = (int8_t)CB2(b, 0xE6);
    c->bme.h6 = (uint8_t)CB2(b, 0xE7);
    c->bme.h7 = (int8_t)CB2(b, 0xE8);

    c->bme.res_heat_range = (b[CALIB1_LEN + CALIB2_LEN + 1] & 0x30) >> 4;
    c->bme.res_heat_val   = (int8_t)b[CALIB1_LEN + CALIB2_LEN + 0];
    c->bme.range_sw_err   = ((int8_t)(b[CALIB1_LEN + CALIB2_LEN + 2] & 0xF0)) / 16;
}

static float bme_comp_temp(telemetry_codec_t *c, uint32_t adc_t)
{
    float var1 = ((adc_t / 16384.0f) - (c->bme.t1 / 1024.0f)) * (float)c->bme.t2;
    float var2 = (((adc_t / 131072.0f) - (c->bme.t1 / 8192.0f)) *
                  ((adc_t / 131072.0f) - (c->bme.t1 / 8192.0f))) *
                 ((float)c->bme.t3 * 16.0f);
    c->bme.t_fine = var1 + var2;
    return c->bme.t_fine / 5120.0f;
}

static float bme_comp_press(telemetry_codec_t *c, uint32_t adc_p)
{
    float var1 = (c->bme.t_fine / 2.0f) - 64000.0f;
    float var2 = var1 * var1 * ((float)c->bme.p6 / 131072.0f);
    var2 = var2 + (var1 * (float)c->bme.p5 * 2.0f);
    var2 = (var2 / 4.0f) + ((float)c->bme.p4 * 65536.0f);
    var1 = ((((float)c->bme.p3 * var1 * var1) / 16384.0f) +
            ((float)c->bme.p2 * var1)) / 524288.0f;
    var1 = (1.0f + (var1 / 32768.0f)) * (float)c->bme.p1;
    if (var1 == 0.0f) return 0.0f;
    float p = 1048576.0f - (float)adc_p;
    p = ((p - (var2 / 4096.0f)) * 6250.0f) / var1;
    var1 = ((float)c->bme.p9 * p * p) / 2147483648.0f;
    var2 = p * ((float)c->bme.p8 / 32768.0f);
    float var3 = (p / 256.0f) * (p / 256.0f) * (p / 256.0f) *
                 ((float)c->bme.p10 / 131072.0f);
    return p + (var1 + var2 + var3 + ((float)c->bme.p7 * 128.0f)) / 16.0f;  /* Pa */
}

static float bme_comp_hum(telemetry_codec_t *c, uint16_t adc_h, float temp_c)
{
    float var1 = (float)adc_h - (((float)c->bme.h1 * 16.0f) +
                                 (((float)c->bme.h3 / 2.0f) * temp_c));
    float var2 = var1 * (((float)c->bme.h2 / 262144.0f) *
                 (1.0f + (((float)c->bme.h4 / 16384.0f) * temp_c) +
                         (((float)c->bme.h5 / 1048576.0f) * temp_c * temp_c)));
    float var3 = (float)c->bme.h6 / 16384.0f;
    float var4 = (float)c->bme.h7 / 2097152.0f;
    float h = var2 + ((var3 + (var4 * temp_c)) * var2 * var2);
    if (h > 100.0f) h = 100.0f;
    else if (h < 0.0f) h = 0.0f;
    return h;
}

static const float k1_range[16] = {0,0,0,0,0,-1,0,-0.8f,0,0,-0.2f,-0.5f,0,-1,0,0};
static const float k2_range[16] = {0,0,0,0,0.1f,0.7f,0,-0.8f,-0.1f,0,0,0,0,0,0,0};

static float bme_comp_gas(telemetry_codec_t *c, uint16_t gas_adc, uint8_t gas_range)
{
    if (gas_range > 15) return 0.0f;
    float var1 = 1340.0f + (5.0f * c->bme.range_sw_err);
    float var2 = var1 * (1.0f + k1_range[gas_range] / 100.0f);
    float var3 = 1.0f + (k2_range[gas_range] / 100.0f);
    float denom = var3 * 0.000000125f * (float)(1u << gas_range) *
                  (((gas_adc - 512.0f) / var2) + 1.0f);
    if (denom == 0.0f) return 0.0f;
    return 1.0f / denom;   /* ohm */
}

/* ── Per-packet decoders ──────────────────────────────────────────────── */

static void proc_mpu(telemetry_codec_t *c, const uint8_t *p)
{
    /* Accelerometer range is mode-dependent: the firmware switches to the wide
     * range during BOOST so the launch spike is not clipped. */
    float sens = (c->state.mode == TELEM_MODE_BOOST) ? c->accel_sens_high
                                                     : c->accel_sens_low;
    if (sens <= 0.0f) sens = 16384.0f;

    c->state.accel_x = rd_i16(p + 0) / sens;
    c->state.accel_y = rd_i16(p + 2) / sens;
    c->state.accel_z = rd_i16(p + 4) / sens;
    c->state.accel_mag = sqrtf(c->state.accel_x * c->state.accel_x +
                               c->state.accel_y * c->state.accel_y +
                               c->state.accel_z * c->state.accel_z);
    /* p + 6..7 is the die temperature, unused here. */
    float g = (c->gyro_sens > 0.0f) ? c->gyro_sens : 32.8f;
    c->state.roll  = rd_i16(p + 8)  / g;
    c->state.pitch = rd_i16(p + 10) / g;
    c->state.yaw   = rd_i16(p + 12) / g;
}

static void proc_bme(telemetry_codec_t *c, const uint8_t *p, uint32_t ts_ms)
{
    /* Raw registers in sensor order: press[3] temp[3] hum[2], MSB first, with
     * the pressure/temperature values left-aligned in 20 bits. */
    uint32_t adc_p = ((uint32_t)p[0] << 12) | ((uint32_t)p[1] << 4) | (p[2] >> 4);
    uint32_t adc_t = ((uint32_t)p[3] << 12) | ((uint32_t)p[4] << 4) | (p[5] >> 4);
    uint16_t adc_h = (uint16_t)((p[6] << 8) | p[7]);

    if (!c->state.have_calib) return;   /* nothing sensible to compute yet */

    c->state.temperature = bme_comp_temp(c, adc_t);        /* also sets t_fine */
    c->state.pressure    = bme_comp_press(c, adc_p) / 1000.0f;   /* Pa -> kPa */
    c->state.humidity    = bme_comp_hum(c, adc_h, c->state.temperature);

    if (c->state.pressure <= 0.0f) return;

    if (c->ground_pressure <= 0.0f) {
        c->ground_pressure = c->state.pressure;
    }

    /* Standard barometric formula, same one json2telemetry.py uses so the live
     * display and the recorded CSV agree. */
    float ratio = c->state.pressure / c->ground_pressure;
    c->state.altitude = 44330.0f * (1.0f - powf(ratio, 1.0f / 5.255f));

    if (c->have_last_alt && ts_ms > c->last_alt_ms) {
        float dt = (ts_ms - c->last_alt_ms) / 1000.0f;
        if (dt > 0.0f) {
            c->state.vertical_velocity = (c->state.altitude - c->last_alt) / dt;
        }
    }
    c->last_alt     = c->state.altitude;
    c->last_alt_ms  = ts_ms;
    c->have_last_alt = true;
}

static void proc_ina(telemetry_codec_t *c, const uint8_t *p)
{
    float shunt_mv = rd_i16(p + 0) / 100.0f;
    c->state.bus_voltage = rd_i16(p + 2) * INA219_MAX_VOLT / INA219_ADC_BITS;
    float shunt_ohm = (c->ina_shunt_ohm > 0.0f) ? c->ina_shunt_ohm : 0.1f;
    c->state.current = shunt_mv / shunt_ohm;                    /* mA */
    c->state.power   = c->state.bus_voltage * c->state.current / 1000.0f;  /* W */
}

/* NMEA fields arrive as fixed-width, NUL-padded ASCII. Copy out and trim. */
static void nmea_field(char *dst, size_t dstsz, const uint8_t *src, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; i < n && j + 1 < dstsz; i++) {
        if (src[i] == '\0' || src[i] == ' ') continue;
        dst[j++] = (char)src[i];
    }
    dst[j] = '\0';
}

static void proc_gps(telemetry_codec_t *c, const uint8_t *p)
{
    char f[16];

    /* Field 0: fix status. 'A' = valid, 'V' = void. */
    if (p[0] != 'A') {
        c->state.gps_fix = false;
        return;
    }
    c->state.gps_fix = true;

    /* Offsets follow frameparser.py's PACKET_DEFS GPS layout:
     * c(1) s(10) s(10) s(10) s(11) c(1) s(12) c(1) s(4) s(8) */
    const uint8_t *speed = p + 1;
    const uint8_t *lat   = p + 31;
    const uint8_t *ns    = p + 42;
    const uint8_t *lon   = p + 43;
    const uint8_t *ew    = p + 55;
    const uint8_t *sats  = p + 56;
    const uint8_t *alt   = p + 60;

    nmea_field(f, sizeof(f), speed, 10);
    if (f[0]) c->state.horizontal_velocity = (float)atof(f) * 1.852f / 3.6f;  /* kn -> m/s */

    /* "DDMM.mmmm" -> degrees. The hemisphere letter carries the sign; without
     * applying it every position south of the equator or west of Greenwich
     * would come out mirrored. */
    nmea_field(f, sizeof(f), lat, 11);
    if (f[0]) {
        double v = atof(f);
        double deg = (double)((int)(v / 100.0));
        c->state.gps_lat = deg + (v - deg * 100.0) / 60.0;
        if (*ns == 'S') c->state.gps_lat = -c->state.gps_lat;
    }

    nmea_field(f, sizeof(f), lon, 12);
    if (f[0]) {
        double v = atof(f);
        double deg = (double)((int)(v / 100.0));
        c->state.gps_lon = deg + (v - deg * 100.0) / 60.0;
        if (*ew == 'W') c->state.gps_lon = -c->state.gps_lon;
    }

    nmea_field(f, sizeof(f), sats, 4);
    if (f[0]) c->state.sat_count = (uint8_t)atoi(f);

    nmea_field(f, sizeof(f), alt, 8);
    if (f[0]) c->state.gps_alt = (float)atof(f);
}

static void proc_calib(telemetry_codec_t *c, const uint8_t *p)
{
    if (p[0] != 1) return;   /* only version 1 is understood */

    bme_parse_calib(c, p + 1);

    uint16_t fs_low   = rd_u16(p + 41);
    uint16_t fs_high  = rd_u16(p + 43);
    uint16_t gyro_fs  = rd_u16(p + 45);
    uint16_t shunt_mo = rd_u16(p + 47);

    if (fs_low)  c->accel_sens_low  = 32768.0f / fs_low;
    if (fs_high) c->accel_sens_high = 32768.0f / fs_high;
    if (gyro_fs) {
        switch (gyro_fs) {
        case 250:  c->gyro_sens = 131.0f;  break;
        case 500:  c->gyro_sens = 65.5f;   break;
        case 1000: c->gyro_sens = 32.8f;   break;
        case 2000: c->gyro_sens = 16.4f;   break;
        default:   c->gyro_sens = 32768.0f / gyro_fs; break;
        }
    }
    if (shunt_mo) c->ina_shunt_ohm = shunt_mo / 1000.0f;

    c->state.have_calib = true;
}

/* ── Frame dispatch ───────────────────────────────────────────────────── */

static void dispatch(telemetry_codec_t *c, uint8_t type,
                     uint32_t ts_ms, const uint8_t *payload)
{
    c->state.timestamp_ms = ts_ms;

    switch (type) {
    case PKT_MPU6050:  proc_mpu(c, payload);         break;
    case PKT_BME680:   proc_bme(c, payload, ts_ms);  break;
    case PKT_GPS:      proc_gps(c, payload);         break;
    case PKT_INA219:   proc_ina(c, payload);         break;
    case PKT_CALIB:    proc_calib(c, payload);       break;
    case PKT_SYSSTATE: c->state.mode = payload[0];   break;
    case PKT_GAS: {
        uint16_t gas_adc  = (uint16_t)((payload[0] << 2) | (payload[1] >> 6));
        uint8_t  gas_rng  = payload[1] & 0x0F;
        if (c->state.have_calib) {
            c->state.gas_resistance = bme_comp_gas(c, gas_adc, gas_rng);
        }
        break;
    }
    default: break;
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

void telemetry_codec_init(telemetry_codec_t *c)
{
    memset(c, 0, sizeof(*c));
    /* Defaults matching frameparser.py's pre-calibration constants, so output
     * is approximately right even before the first CALIB frame arrives. */
    c->accel_sens_low  = 16384.0f;   /* +/-2 g  */
    c->accel_sens_high = 2048.0f;    /* +/-16 g */
    c->gyro_sens       = 32.8f;      /* +/-1000 dps */
    c->ina_shunt_ohm   = 0.1f;
    c->state.mode      = TELEM_MODE_INIT;
}

void telemetry_codec_feed(telemetry_codec_t *c, const uint8_t *data, size_t len)
{
    if (c == NULL || data == NULL) return;

    while (len > 0) {
        /* Refill the working buffer. If it is somehow full without yielding a
         * frame, the contents cannot be valid - drop the oldest half and carry
         * on rather than wedging. */
        size_t space = sizeof(c->buf) - c->used;
        if (space == 0) {
            memmove(c->buf, c->buf + sizeof(c->buf) / 2, sizeof(c->buf) / 2);
            c->used = sizeof(c->buf) / 2;
            space = c->used;
        }
        size_t take = (len < space) ? len : space;
        memcpy(c->buf + c->used, data, take);
        c->used += take;
        data    += take;
        len     -= take;

        /* Drain every complete frame currently buffered. */
        size_t pos = 0;
        while (1) {
            /* Hunt for the sync word. */
            size_t sync = pos;
            bool found = false;
            while (sync + 3 <= c->used) {
                if (c->buf[sync] == 0xAA && c->buf[sync + 1] == 0xAA &&
                    c->buf[sync + 2] == 0xAA) { found = true; break; }
                sync++;
            }
            if (!found) {
                /* Keep the last two bytes: a sync word may straddle the join. */
                pos = (c->used >= 2) ? c->used - 2 : 0;
                break;
            }
            pos = sync;

            if (c->used - pos < HEADER_SIZE) break;   /* need more bytes */

            uint8_t type = c->buf[pos + 3];
            int psize = payload_size_for(type);
            if (psize < 0) {
                /* Not a real frame - sync bytes occurring inside payload data.
                 * Step one byte and hunt again. */
                pos++;
                continue;
            }

            size_t total = HEADER_SIZE + (size_t)psize + FOOTER_SIZE;
            if (c->used - pos < total) break;         /* need more bytes */

            uint16_t want = rd_u16(c->buf + pos + total - FOOTER_SIZE);
            uint16_t got  = crc16_ccitt(c->buf + pos, total - FOOTER_SIZE);
            if (want != got) {
                c->state.frames_crc_fail++;
                pos++;                                 /* resync */
                continue;
            }

            dispatch(c, type, rd_u32(c->buf + pos + 4), c->buf + pos + HEADER_SIZE);
            c->state.frames_ok++;
            pos += total;
        }

        /* Retain whatever is left for the next call. */
        if (pos > 0) {
            memmove(c->buf, c->buf + pos, c->used - pos);
            c->used -= pos;
        }
    }
}

const char *telemetry_mode_name(uint8_t mode)
{
    static const char *names[TELEM_MODE_COUNT] = {
        "INIT", "POST", "SNSCHK", "ARMED", "BOOST", "COAST"
    };
    return (mode < TELEM_MODE_COUNT) ? names[mode] : "?";
}
