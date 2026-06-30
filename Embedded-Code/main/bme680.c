/**
 * @file bme680.c
 * @brief Bosch BME680 driver — raw logging + flight-mode-adaptive profiles.
 *
 * Mirrors the mpu6050/ina219 pattern:
 *   - I2C1 (shared with INA219). Auto-detect 0x76/0x77, verify chip id 0x61.
 *   - Logs RAW bytes; compensation is done offline from the calib blob dumped
 *     once at boot (serial hex + a one-time SBIT_RESERVED0 frame in the SD log).
 *   - Forced mode: each sample triggers one measurement, polls new_data, reads field 0.
 *   - Profile follows context.mode:
 *        POST / ARMED : T/P/H, gas OFF, P x4 + light IIR, ~BME680_PERIOD_PAD_MS
 *        BOOST        : T/P/H, gas OFF, P x2 + IIR OFF (low lag), ~BME680_PERIOD_FLIGHT_MS
 *        COAST        : T/P/H at flight rate, + one gas measurement every
 *                       BME680_GAS_INTERVAL_MS (interleaved, to keep altitude fast)
 *
 * Register map is the BME68x layout (NOT BME280's 0xF7 block).
 */

#include "bme680.h"
#include "i2c_bus.h"
#include "systemp2i.h"
#include "frame_logger.h"
#include "health_monitoring.h"
#include "flight_stats.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "bme680";

/* ── Register map ─────────────────────────────────────────── */
#define REG_RESET           0xE0
#define VAL_RESET           0xB6
#define REG_CHIP_ID         0xD0
#define REG_MEAS_STATUS     0x1D    /* bit7 new_data_0 */
#define REG_PRESS_MSB       0x1F    /* press(3) temp(3) hum(2) = 8 contiguous bytes */
#define REG_GAS_R_MSB       0x2A    /* gas(2) */
#define REG_RES_HEAT_0      0x5A
#define REG_GAS_WAIT_0      0x64
#define REG_CTRL_GAS_0      0x70    /* bit3 heat_off (0 = heater on) */
#define REG_CTRL_GAS_1      0x71    /* bit4 run_gas, [3:0] nb_conv */
#define REG_CTRL_HUM        0x72    /* [2:0] osrs_h */
#define REG_CTRL_MEAS       0x74    /* [7:5] osrs_t, [4:2] osrs_p, [1:0] mode */
#define REG_CONFIG          0x75    /* [4:2] IIR filter */

#define MODE_FORCED         0x01

#define OSRS_X1             0x01
#define OSRS_X2             0x02
#define OSRS_X4             0x03
#define FILTER_OFF          0x00
#define FILTER_COEFF_3      0x02

#define RUN_GAS             0x10    /* ctrl_gas_1 bit4 */

#define BME680_MAX_FAILED_ATTEMPTS  60

/* ── Calibration blob (raw — parsed offline) ──────────────── */
#define CALIB1_ADDR   0x8A
#define CALIB1_LEN    23            /* 0x8A..0xA0 */
#define CALIB2_ADDR   0xE1
#define CALIB2_LEN    14            /* 0xE1..0xEE */
#define CALIB_EXTRA   3             /* res_heat_val(0x00), res_heat_range(0x02), range_sw_err(0x04) */
#define CALIB_LEN     (CALIB1_LEN + CALIB2_LEN + CALIB_EXTRA)   /* 40 bytes */

static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t  s_calib[CALIB_LEN];

/* Profile state (set by bme680_apply_profile, used by bme680_measure) */
static uint8_t  s_ctrl_meas  = (OSRS_X1 << 5) | (OSRS_X4 << 2); /* osrs_t/osrs_p, mode OR'd at trigger */
static uint32_t s_period_ms  = 100;
static bool     s_gas_allowed = false;
static uint8_t  s_res_heat   = 0;   /* computed heater set-point code */

/* ── Gas heater set-point from calibration (Bosch formula) ── */
static uint8_t bme680_calc_res_heat(int16_t target_c, int16_t amb_c)
{
    int8_t   par_g1        = (int8_t)  s_calib[CALIB1_LEN + (0xED - CALIB2_ADDR)];
    int16_t  par_g2        = (int16_t)((s_calib[CALIB1_LEN + (0xEC - CALIB2_ADDR)] << 8) |
                                        s_calib[CALIB1_LEN + (0xEB - CALIB2_ADDR)]);
    int8_t   par_g3        = (int8_t)  s_calib[CALIB1_LEN + (0xEE - CALIB2_ADDR)];
    uint8_t  res_heat_range = (s_calib[CALIB1_LEN + CALIB2_LEN + 1] & 0x30) >> 4;
    int8_t   res_heat_val   = (int8_t) s_calib[CALIB1_LEN + CALIB2_LEN + 0];

    double var1 = (par_g1 / 16.0) + 49.0;
    double var2 = ((par_g2 / 32768.0) * 0.0005) + 0.00235;
    double var3 = par_g3 / 1024.0;
    double var4 = var1 * (1.0 + (var2 * (double)target_c));
    double var5 = var4 + (var3 * (double)amb_c);
    double res  = 3.4 * ((var5 * (4.0 / (4.0 + res_heat_range)) *
                          (1.0 / (1.0 + (res_heat_val * 0.002)))) - 25.0);
    if (res < 0)   res = 0;
    if (res > 255) res = 255;
    return (uint8_t)res;
}

static esp_err_t bme680_read_calib(void)
{
    esp_err_t ret;
    ret = i2c_bus_read_bytes(s_dev, CALIB1_ADDR, s_calib, CALIB1_LEN);
    if (ret != ESP_OK) return ret;
    ret = i2c_bus_read_bytes(s_dev, CALIB2_ADDR, s_calib + CALIB1_LEN, CALIB2_LEN);
    if (ret != ESP_OK) return ret;
    /* extra single registers (heater range/val + range-switching error) */
    i2c_bus_read_bytes(s_dev, 0x00, &s_calib[CALIB1_LEN + CALIB2_LEN + 0], 1);
    i2c_bus_read_bytes(s_dev, 0x02, &s_calib[CALIB1_LEN + CALIB2_LEN + 1], 1);
    i2c_bus_read_bytes(s_dev, 0x04, &s_calib[CALIB1_LEN + CALIB2_LEN + 2], 1);
    return ESP_OK;
}

/* Reconfigure the sensor for a flight mode. Returns true if gas is allowed. */
static bool bme680_apply_profile(uint32_t mode)
{
    uint8_t osrs_t, osrs_p, osrs_h, filter;

    switch (mode) {
    case MODE_BOOST:
        osrs_t = OSRS_X1; osrs_p = OSRS_X2; osrs_h = OSRS_X1; filter = FILTER_OFF;
        s_period_ms = BME680_PERIOD_FLIGHT_MS; s_gas_allowed = false;
        break;
    case MODE_COAST:
        osrs_t = OSRS_X1; osrs_p = OSRS_X2; osrs_h = OSRS_X1; filter = FILTER_OFF;
        s_period_ms = BME680_PERIOD_FLIGHT_MS; s_gas_allowed = true;
        break;
    case MODE_POST:
    case MODE_ARMED:
    default:
        osrs_t = OSRS_X1; osrs_p = OSRS_X4; osrs_h = OSRS_X1; filter = FILTER_COEFF_3;
        s_period_ms = BME680_PERIOD_PAD_MS; s_gas_allowed = false;
        break;
    }

    i2c_bus_write_byte(s_dev, REG_CTRL_HUM, osrs_h);
    i2c_bus_write_byte(s_dev, REG_CONFIG,  filter << 2);
    s_ctrl_meas = (osrs_t << 5) | (osrs_p << 2);   /* mode bits added at each trigger */

    if (s_gas_allowed) {
        i2c_bus_write_byte(s_dev, REG_GAS_WAIT_0, 0x59);        /* ~100 ms heater */
        i2c_bus_write_byte(s_dev, REG_RES_HEAT_0, s_res_heat);  /* heater set-point */
        i2c_bus_write_byte(s_dev, REG_CTRL_GAS_0, 0x00);        /* heater enabled */
    }

    ESP_LOGI(TAG, "profile: mode=%lu osrs_p=%u filter=%u gas=%d period=%lums",
             (unsigned long)mode, osrs_p, filter, (int)s_gas_allowed, (unsigned long)s_period_ms);
    return s_gas_allowed;
}

/* Trigger one forced measurement and read field 0. */
static esp_err_t bme680_measure(struct BME680Raw *out, bool with_gas)
{
    esp_err_t ret;

    ret = i2c_bus_write_byte(s_dev, REG_CTRL_GAS_1, with_gas ? RUN_GAS : 0x00);
    if (ret != ESP_OK) return ret;

    ret = i2c_bus_write_byte(s_dev, REG_CTRL_MEAS, s_ctrl_meas | MODE_FORCED);
    if (ret != ESP_OK) return ret;

    /* Wait for the measurement to actually complete before reading. Polling the
     * new_data flag alone is unsafe: it can still be set from the previous
     * (gas-less) cycle, so we'd latch a stale result and the gas registers would
     * read their 0x0004 default. A gas cycle also runs the heater (~100 ms), so
     * delay the bulk of the conversion first, then confirm fresh new_data. */
    vTaskDelay(pdMS_TO_TICKS(with_gas ? 180 : 15));

    uint8_t status = 0;
    int tries = 20;
    do {
        ret = i2c_bus_read_bytes(s_dev, REG_MEAS_STATUS, &status, 1);
        if (ret != ESP_OK) return ret;
        if (status & 0x80) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    } while (--tries > 0);
    if (!(status & 0x80)) return ESP_ERR_TIMEOUT;

    /* press(3) + temp(3) + hum(2) = first 8 bytes of the struct */
    ret = i2c_bus_read_bytes(s_dev, REG_PRESS_MSB, (uint8_t *)out, 8);
    if (ret != ESP_OK) return ret;

    if (with_gas) {
        ret = i2c_bus_read_bytes(s_dev, REG_GAS_R_MSB, out->gas, 2);
        if (ret != ESP_OK) return ret;
    } else {
        out->gas[0] = 0;
        out->gas[1] = 0;
    }
    return ESP_OK;
}

/* ── Public API ───────────────────────────────────────────── */
esp_err_t bme680_init(i2c_master_bus_handle_t bus)
{
    const uint8_t addrs[2] = { BME680_ADDR_LOW, BME680_ADDR_HIGH };
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    uint8_t   id  = 0;

    for (int i = 0; i < 2; i++) {
        if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
        if (i2c_bind(bus, &s_dev, addrs[i]) != ESP_OK) continue;
        if (i2c_bus_read_bytes(s_dev, REG_CHIP_ID, &id, 1) == ESP_OK &&
            id == BME680_CHIP_ID) {
            ESP_LOGI(TAG, "BME680 online at 0x%02X (chip id 0x%02X)", addrs[i], id);
            ret = ESP_OK;
            break;
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BME680 not found at 0x76/0x77 (last id 0x%02X)", id);
        return ret;
    }

    /* Soft reset, then read calibration and pre-compute the heater set-point. */
    i2c_bus_write_byte(s_dev, REG_RESET, VAL_RESET);
    vTaskDelay(pdMS_TO_TICKS(10));

    ret = bme680_read_calib();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "calibration read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_res_heat = bme680_calc_res_heat(BME680_HEATER_TEMP_C, BME680_HEATER_AMB_C);

    /* Dump calib so post-processing can compensate (40 raw bytes). */
    ESP_LOGI(TAG, "calibration blob (%d bytes), res_heat=0x%02X:", CALIB_LEN, s_res_heat);
    ESP_LOG_BUFFER_HEX(TAG, s_calib, CALIB_LEN);

    bme680_apply_profile(MODE_POST);   /* start in the pad profile */
    return ESP_OK;
}

void bme680_task(void *arg)
{
    struct TaskParams *tp = (struct TaskParams *)arg;
    struct BME680Raw  raw;
    uint32_t last_mode    = 0xFFFFFFFFu;   /* force first-cycle configure */
    uint16_t hm_counter   = 0;
    uint8_t  attempts     = 0;
    int64_t  last_gas_us  = 0;
    int64_t  last_print_us = 0;
    uint8_t  last_gas[2]  = {0, 0};
    bool     calib_logged = false;

    while (1) {
        uint32_t mode = tp->context->mode;
        if (mode != last_mode) {
            bme680_apply_profile(mode);
            last_mode = mode;
        }

        /* Gas only in COAST, interleaved so T/P/H stays fast. */
        bool do_gas = false;
        if (s_gas_allowed) {
            int64_t now = esp_timer_get_time();
            if (now - last_gas_us >= (int64_t)BME680_GAS_INTERVAL_MS * 1000) {
                do_gas = true;
                last_gas_us = now;
            }
        }

        if (bme680_measure(&raw, do_gas) != ESP_OK) {
            if (++attempts >= BME680_MAX_FAILED_ATTEMPTS) {
                ESP_LOGE(TAG, "BME680 unresponsive — stopping task");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        attempts = 0;
        flight_stats_tick(STAT_BME680);
        if (do_gas) { last_gas[0] = raw.gas[0]; last_gas[1] = raw.gas[1]; }

        switch (mode) {
        case MODE_POST:
        case MODE_ARMED:
            if (++hm_counter >= BME680_HM_SKIP_SAMPLES) {
                hm_counter = 0;
                hm_send(tp->hm_buffer, SBIT_BME680, (uint8_t *)&raw, sizeof(raw));
            }
            break;

        case MODE_BOOST:
        case MODE_COAST:
            /* Make the SD log self-describing: calib first, once. */
            if (!calib_logged) {
                write_to_ring_buffer(tp->log_buffer, SBIT_RESERVED0, s_calib, CALIB_LEN);
                calib_logged = true;
            }
            write_to_ring_buffer(tp->log_buffer, SBIT_BME680, (uint8_t *)&raw, sizeof(raw));
            break;
        }

        /* BENCH: echo to serial at ~2 Hz in every mode so the gas channel is
         * visible when you switch to COAST (gas= is the latest gas reading;
         * 2nd byte bit4=heat_stab, bit5=gas_valid). Remove for flight. */
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_print_us >= 500000) {
            last_print_us = now_us;
            const char *mn = (mode == MODE_COAST) ? "COAST" :
                             (mode == MODE_BOOST) ? "BOOST" :
                             (mode == MODE_ARMED) ? "ARMED" : "PAD";
            ESP_LOGI(TAG, "[%s] P=%02X%02X%02X T=%02X%02X%02X H=%02X%02X gas=%02X%02X",
                     mn, raw.press[0], raw.press[1], raw.press[2],
                     raw.temp[0], raw.temp[1], raw.temp[2],
                     raw.hum[0], raw.hum[1], last_gas[0], last_gas[1]);
        }

        vTaskDelay(pdMS_TO_TICKS(s_period_ms));
    }

    vTaskDelete(NULL);
}
