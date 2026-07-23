/**
 * @file bme680.c
 * @brief Bosch BME680 driver — RAW logging + flight-mode profiles.
 *
 *   - I2C1 (shared with INA219). Auto-detect 0x76/0x77, verify chip id 0x61.
 *   - Logs RAW registers; ground applies the Bosch compensation from the
 *     calibration frame (SBIT_CALIB carries the raw calib blob):
 *         SBIT_BME680 = 8 bytes (press[3] temp[3] hum[2], register order)
 *   - Gas: its own SBIT_GAS frame, 2 raw bytes (gas_r_msb, gas_r_lsb), emitted in
 *     POST/ARMED/COAST (interleaved ~1 Hz, only when the reading is valid). OFF in BOOST.
 *   - The on-device compensation below is kept ONLY for the bench serial echo.
 *   - Forced mode: each sample triggers one measurement, waits out the conversion
 *     (the gas heater needs ~100 ms), reads field 0.
 *   - Profiles follow context.mode:
 *        POST/ARMED : T/P/H pad rate + gas
 *        BOOST      : T/P/H fast, IIR off, NO gas (low-lag altitude)
 *        COAST      : T/P/H fast + gas
 *
 * Register map / compensation are the BME68x datasheet (float) algorithms.
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

#define GAS_VALID_BIT       0x20    /* gas_r_lsb bit5 */

#define BME680_MAX_FAILED_ATTEMPTS  60

/* ── Calibration blob (raw) + parsed coefficients ─────────── */
#define CALIB1_ADDR   0x8A
#define CALIB1_LEN    23            /* 0x8A..0xA0 */
#define CALIB2_ADDR   0xE1
#define CALIB2_LEN    14            /* 0xE1..0xEE */
#define CALIB_EXTRA   3             /* res_heat_val(0x00), res_heat_range(0x02), range_sw_err(0x04) */
#define CALIB_LEN     (CALIB1_LEN + CALIB2_LEN + CALIB_EXTRA)   /* 40 bytes */
#define CB1(reg)      (s_calib[(reg) - CALIB1_ADDR])
#define CB2(reg)      (s_calib[CALIB1_LEN + ((reg) - CALIB2_ADDR)])

static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_calib[CALIB_LEN];

static struct {
    uint16_t t1; int16_t t2; int8_t t3;
    uint16_t p1; int16_t p2; int8_t p3; int16_t p4, p5; int8_t p6, p7; int16_t p8, p9; uint8_t p10;
    uint16_t h1, h2; int8_t h3, h4, h5; uint8_t h6; int8_t h7;
    int8_t   g1; int16_t g2; int8_t g3;
    uint8_t  res_heat_range; int8_t res_heat_val; int8_t range_sw_err;
    float    t_fine;            /* carried from temp comp into pressure comp */
} cal;

/* Compensated T/P/H payload — matches the protocol's BME680 = '<3f' / 12 B. */
struct __attribute__((packed)) bme680_tph_t {
    float temperature;  /* °C  */
    float pressure;     /* Pa  */
    float humidity;     /* %RH */
};

/* Profile state (set by bme680_apply_profile, used by bme680_measure) */
static uint8_t  s_ctrl_meas   = (OSRS_X1 << 5) | (OSRS_X4 << 2);
static uint32_t s_period_ms   = 100;
static bool     s_gas_allowed = false;
static uint8_t  s_res_heat    = 0;

/* Bosch gas-range correction lookup tables (BME680 float gas calc). */
static const float k1_range[16] = {0,0,0,0,0,-1,0,-0.8f,0,0,-0.2f,-0.5f,0,-1,0,0};
static const float k2_range[16] = {0,0,0,0,0.1f,0.7f,0,-0.8f,-0.1f,0,0,0,0,0,0,0};

/* ── Calibration ──────────────────────────────────────────── */
static void bme680_parse_calib(void)
{
    cal.t1 = (uint16_t)((CB2(0xEA) << 8) | CB2(0xE9));
    cal.t2 = (int16_t)((CB1(0x8B) << 8) | CB1(0x8A));
    cal.t3 = (int8_t)CB1(0x8C);

    cal.p1  = (uint16_t)((CB1(0x8F) << 8) | CB1(0x8E));
    cal.p2  = (int16_t)((CB1(0x91) << 8) | CB1(0x90));
    cal.p3  = (int8_t)CB1(0x92);
    cal.p4  = (int16_t)((CB1(0x95) << 8) | CB1(0x94));
    cal.p5  = (int16_t)((CB1(0x97) << 8) | CB1(0x96));
    cal.p6  = (int8_t)CB1(0x99);
    cal.p7  = (int8_t)CB1(0x98);
    cal.p8  = (int16_t)((CB1(0x9D) << 8) | CB1(0x9C));
    cal.p9  = (int16_t)((CB1(0x9F) << 8) | CB1(0x9E));
    cal.p10 = (uint8_t)CB1(0xA0);

    cal.h1 = (uint16_t)((CB2(0xE3) << 4) | (CB2(0xE2) & 0x0F));
    cal.h2 = (uint16_t)((CB2(0xE1) << 4) | (CB2(0xE2) >> 4));
    cal.h3 = (int8_t)CB2(0xE4);
    cal.h4 = (int8_t)CB2(0xE5);
    cal.h5 = (int8_t)CB2(0xE6);
    cal.h6 = (uint8_t)CB2(0xE7);
    cal.h7 = (int8_t)CB2(0xE8);

    cal.g1 = (int8_t)CB2(0xED);
    cal.g2 = (int16_t)((CB2(0xEC) << 8) | CB2(0xEB));
    cal.g3 = (int8_t)CB2(0xEE);

    cal.res_heat_range = (s_calib[CALIB1_LEN + CALIB2_LEN + 1] & 0x30) >> 4;
    cal.res_heat_val   = (int8_t)s_calib[CALIB1_LEN + CALIB2_LEN + 0];
    cal.range_sw_err   = ((int8_t)(s_calib[CALIB1_LEN + CALIB2_LEN + 2] & 0xF0)) / 16;
}

static esp_err_t bme680_read_calib(void)
{
    esp_err_t ret;
    ret = i2c_bus_read_bytes(s_dev, CALIB1_ADDR, s_calib, CALIB1_LEN);
    if (ret != ESP_OK) return ret;
    ret = i2c_bus_read_bytes(s_dev, CALIB2_ADDR, s_calib + CALIB1_LEN, CALIB2_LEN);
    if (ret != ESP_OK) return ret;
    i2c_bus_read_bytes(s_dev, 0x00, &s_calib[CALIB1_LEN + CALIB2_LEN + 0], 1);
    i2c_bus_read_bytes(s_dev, 0x02, &s_calib[CALIB1_LEN + CALIB2_LEN + 1], 1);
    i2c_bus_read_bytes(s_dev, 0x04, &s_calib[CALIB1_LEN + CALIB2_LEN + 2], 1);
    bme680_parse_calib();
    return ESP_OK;
}

/* ── Bosch float compensation (BME68x datasheet) ──────────── */
static float bme680_comp_temp(uint32_t adc_t)
{
    float var1 = ((adc_t / 16384.0f) - (cal.t1 / 1024.0f)) * (float)cal.t2;
    float var2 = (((adc_t / 131072.0f) - (cal.t1 / 8192.0f)) *
                  ((adc_t / 131072.0f) - (cal.t1 / 8192.0f))) * ((float)cal.t3 * 16.0f);
    cal.t_fine = var1 + var2;
    return cal.t_fine / 5120.0f;
}

static float bme680_comp_press(uint32_t adc_p)
{
    float var1 = (cal.t_fine / 2.0f) - 64000.0f;
    float var2 = var1 * var1 * ((float)cal.p6 / 131072.0f);
    var2 = var2 + (var1 * (float)cal.p5 * 2.0f);
    var2 = (var2 / 4.0f) + ((float)cal.p4 * 65536.0f);
    var1 = ((((float)cal.p3 * var1 * var1) / 16384.0f) + ((float)cal.p2 * var1)) / 524288.0f;
    var1 = (1.0f + (var1 / 32768.0f)) * (float)cal.p1;
    if (var1 == 0.0f) return 0.0f;
    float p = 1048576.0f - (float)adc_p;
    p = ((p - (var2 / 4096.0f)) * 6250.0f) / var1;
    var1 = ((float)cal.p9 * p * p) / 2147483648.0f;
    var2 = p * ((float)cal.p8 / 32768.0f);
    float var3 = (p / 256.0f) * (p / 256.0f) * (p / 256.0f) * ((float)cal.p10 / 131072.0f);
    return p + (var1 + var2 + var3 + ((float)cal.p7 * 128.0f)) / 16.0f;   /* Pa */
}

static float bme680_comp_hum(uint16_t adc_h, float temp_c)
{
    float var1 = (float)adc_h - (((float)cal.h1 * 16.0f) + (((float)cal.h3 / 2.0f) * temp_c));
    float var2 = var1 * (((float)cal.h2 / 262144.0f) *
                 (1.0f + (((float)cal.h4 / 16384.0f) * temp_c) +
                         (((float)cal.h5 / 1048576.0f) * temp_c * temp_c)));
    float var3 = (float)cal.h6 / 16384.0f;
    float var4 = (float)cal.h7 / 2097152.0f;
    float h = var2 + ((var3 + (var4 * temp_c)) * var2 * var2);
    if (h > 100.0f) h = 100.0f;
    else if (h < 0.0f) h = 0.0f;
    return h;   /* %RH */
}

static float bme680_comp_gas(uint16_t gas_adc, uint8_t gas_range)
{
    float var1 = 1340.0f + (5.0f * (float)cal.range_sw_err);
    float var2 = var1 * (1.0f + k1_range[gas_range] / 100.0f);
    float var3 = 1.0f + (k2_range[gas_range] / 100.0f);
    return 1.0f / (var3 * 0.000000125f * (float)(1 << gas_range) *
                   ((((float)gas_adc - 512.0f) / var2) + 1.0f));   /* ohm */
}

/* Gas heater set-point code from calibration (Bosch formula). */
static uint8_t bme680_calc_res_heat(int16_t target_c, int16_t amb_c)
{
    double var1 = (cal.g1 / 16.0) + 49.0;
    double var2 = ((cal.g2 / 32768.0) * 0.0005) + 0.00235;
    double var3 = cal.g3 / 1024.0;
    double var4 = var1 * (1.0 + (var2 * (double)target_c));
    double var5 = var4 + (var3 * (double)amb_c);
    double res  = 3.4 * ((var5 * (4.0 / (4.0 + cal.res_heat_range)) *
                          (1.0 / (1.0 + (cal.res_heat_val * 0.002)))) - 25.0);
    if (res < 0)   res = 0;
    if (res > 255) res = 255;
    return (uint8_t)res;
}

/* Reconfigure the sensor for a flight mode. Returns true if gas is enabled. */
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
        s_period_ms = BME680_PERIOD_PAD_MS; s_gas_allowed = true;
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

/* Trigger one forced measurement and read field 0 (raw). */
static esp_err_t bme680_measure(struct BME680Raw *out, bool with_gas)
{
    esp_err_t ret;

    ret = i2c_bus_write_byte(s_dev, REG_CTRL_GAS_1, with_gas ? RUN_GAS : 0x00);
    if (ret != ESP_OK) return ret;

    ret = i2c_bus_write_byte(s_dev, REG_CTRL_MEAS, s_ctrl_meas | MODE_FORCED);
    if (ret != ESP_OK) return ret;

    /* Wait out the conversion before reading (a gas cycle runs the heater ~100 ms),
     * otherwise we latch a stale new_data flag from the previous cycle. */
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

    ret = i2c_bus_read_bytes(s_dev, REG_PRESS_MSB, (uint8_t *)out, 8);   /* press+temp+hum */
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
const uint8_t *bme680_get_calib_blob(uint16_t *len)
{
    _Static_assert(CALIB_LEN == BME680_CALIB_BLOB_LEN, "calib blob length mismatch");
    if (len) *len = CALIB_LEN;
    return s_calib;
}

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

    i2c_bus_write_byte(s_dev, REG_RESET, VAL_RESET);
    vTaskDelay(pdMS_TO_TICKS(10));

    ret = bme680_read_calib();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "calibration read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_res_heat = bme680_calc_res_heat(BME680_HEATER_TEMP_C, BME680_HEATER_AMB_C);
    ESP_LOGI(TAG, "calibration parsed, res_heat=0x%02X", s_res_heat);

    bme680_apply_profile(MODE_POST);   /* start in the pad profile */
    return ESP_OK;
}

void bme680_task(void *arg)
{
    struct TaskParams *tp = (struct TaskParams *)arg;
    struct BME680Raw  raw;
    uint32_t last_mode     = 0xFFFFFFFFu;   /* force first-cycle configure */
    uint16_t hm_counter    = 0;
    uint8_t  attempts      = 0;
    int64_t  last_gas_us   = 0;
    int64_t  last_print_us = 0;
    float    last_gas_ohm  = 0.0f;

    while (1) {
        uint32_t mode = tp->context->mode;
        if (mode != last_mode) {
            bme680_apply_profile(mode);
            last_mode = mode;
        }

        /* Gas interleaved ~1 Hz when enabled, so T/P/H stays fast. */
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

        /* Raw registers -> compensated values. */
        uint32_t adc_t = ((uint32_t)raw.temp[0]  << 12) | ((uint32_t)raw.temp[1]  << 4) | (raw.temp[2]  >> 4);
        uint32_t adc_p = ((uint32_t)raw.press[0] << 12) | ((uint32_t)raw.press[1] << 4) | (raw.press[2] >> 4);
        uint16_t adc_h = ((uint16_t)raw.hum[0]   << 8)  |  raw.hum[1];

        /* T/P/H frame: 8 RAW bytes (press[3] temp[3] hum[2]); ground compensates
         * using the calibration frame. Always captured to the circular buffer;
         * throttled telemetry on the pad. */
        write_to_ring_buffer(tp->log_buffer, SBIT_BME680, &raw, 8);
        if (mode == MODE_POST || mode == MODE_ARMED) {
            if (++hm_counter >= BME680_HM_SKIP_SAMPLES) {
                hm_counter = 0;
                hm_send(tp->hm_buffer, SBIT_BME680, (uint8_t *)&raw, 8);
            }
        }

        /* Gas: its own frame (2 raw bytes), only when measured and flagged valid. */
        if (do_gas && (raw.gas[1] & GAS_VALID_BIT)) {
            write_to_ring_buffer(tp->log_buffer, SBIT_GAS, raw.gas, 2);   /* always -> circular buffer */
            if (mode == MODE_POST || mode == MODE_ARMED) {
                hm_send(tp->hm_buffer, SBIT_GAS, raw.gas, 2);             /* + live telemetry */
            }
        }

        /* Compensated values: BENCH ECHO ONLY (the log stays raw). */
        struct bme680_tph_t tph;
        tph.temperature = bme680_comp_temp(adc_t);   /* also sets cal.t_fine */
        tph.pressure    = bme680_comp_press(adc_p);
        tph.humidity    = bme680_comp_hum(adc_h, tph.temperature);
        if (do_gas && (raw.gas[1] & GAS_VALID_BIT)) {
            uint16_t gas_adc   = ((uint16_t)raw.gas[0] << 2) | (raw.gas[1] >> 6);
            uint8_t  gas_range = raw.gas[1] & 0x0F;
            last_gas_ohm = bme680_comp_gas(gas_adc, gas_range);
        }

        /* BENCH echo ~2 Hz (compensated, all modes). Remove for flight. */
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_print_us >= 500000) {
            last_print_us = now_us;
            const char *mn = (mode == MODE_COAST) ? "COAST" :
                             (mode == MODE_BOOST) ? "BOOST" :
                             (mode == MODE_ARMED) ? "ARMED" : "PAD";
            ESP_LOGI(TAG, "[%s] T=%.2f C  P=%.0f Pa  H=%.1f %%  gas=%.0f ohm",
                     mn, tph.temperature, tph.pressure, tph.humidity, last_gas_ohm);
        }

        vTaskDelay(pdMS_TO_TICKS(s_period_ms));
    }

    vTaskDelete(NULL);
}
