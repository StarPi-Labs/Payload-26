/**
 * @file bme680.c
 * @brief BME680 temperature / pressure / humidity driver (stub).
 * TODO: check this registers
 * Reads 8 raw bytes from register 0xF7:
 *   press_msb(1) press_lsb(1) press_xlsb(1)
 *   temp_msb(1)  temp_lsb(1)  temp_xlsb(1)
 *   hum_msb(1)   hum_lsb(1)
 *
 * TODO: (POST-PROCESSING) Add compensation formulas from the BME680 datasheet for
 *       calibrated temperature (°C), pressure (Pa), humidity (%RH).
 *       
 */

#include "bme680.h"
#include "i2c_bus.h"
#include "esp_log.h"

static const char *TAG = "bme680";

/* ── Register map ─────────────────────────────────────────── */
#define BME680_ADDR         0x76
#define REG_CHIP_ID         0xD0    /* Expected: 0x61 */
#define VAL_CHIP_ID         0x61
#define REG_CTRL_HUM        0x72
#define REG_CTRL_MEAS       0x74
#define REG_CTRL_GAS_1      0x71
#define REG_CONFIG          0xF5
#define REG_DATA_START      0xF7    /* 8 bytes: press(3) + temp(3) + hum(2) */

/* BME680 Configuration */
#define HUM_SAMPLINGx1      0x01
#define TEMP_SAMPLINGx1     0x20
#define PRES_SAMPLINGx1     0x04
#define FORCE_MODE          0x01
#define TRIGGER_MEASURE     (TEMP_SAMPLINGx1 | PRES_SAMPLINGx1 | FORCE_MODE)
#define FILTER_OFF          0x00
#define MEASURE_DURATION_MS 10      // This varies based on the oversampling configuration
                                    // Read the datasheet for further explanation.

#define BME680_DATA_LEN     8       /* TODO: check data length for BME680 */

/* Configuration */
#define DISABLE_GAS         0x00
#define OSRS_TEM_PRES       0b00100100
#define OSRS_HUM            0b00000001
#define FILTER_OFF          0x00

static i2c_master_dev_handle_t s_dev = NULL;
/* ── Public API ───────────────────────────────────────────── */

esp_err_t bme680_int(i2c_master_bus_handle_t bus) {
    esp_err_t ret = i2c_bind(bus, &s_dev, BME680_ADDR);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add BME680: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Ping device */
    uint8_t chip_id = 0;
    ret = i2c_bus_read_bytes(s_dev, REG_CHIP_ID, &chip_id, 1);

    /* Was connection possible? */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect with sensor: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Is it the right sensor? */
    if (chip_id != VAL_CHIP_ID) {
        ESP_LOGE(TAG, "Wrong Sensor: Expected 0x%02X, got 0x%02X", VAL_CHIP_ID, chip_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ret;
}

esp_err_t bme680_config(void)
{
    /* TODO: Check this configuration for BME680 */
    /* Humidity oversampling ×1 (must be set before ctrl_meas) */
    ret = i2c_bus_write_byte(s_dev, REG_CTRL_HUM, HUM_SAMPLINGx1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed Humidity sampling rate.");
        return ret;
    }

    /* temp oversample ×1, press oversample ×1, Forced mode*/
    ret = i2c_bus_write_byte(s_dev, REG_CTRL_MEAS, TRIGGER_MEASURE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set temp and pressure sample modes.");
        return ret;
    }

    /* Standby 0.5 ms, filter off */
    ret = i2c_bus_write_byte(s_dev, REG_CONFIG, FILTER_OFF);
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}

/* read BME680 in fsm */
esp_err_t read_bme680(uint8_t *out_data, uint8_t attempts) {
    esp_err_t ret;
    ret = i2c_bus_write_byte(s_dev, REG_CTRL_MEAS, TRIGGER_MEASURE);

    if (ret != ESP_OK) {
        attempts++;
        return attempts;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    ret = i2c_bus_read_bytes(s_dev, REG_DATA_START, out_data, BME680_DATA_LEN);
    if (ret != ESP_OK) {
        attempts++;
        return attempts;
    }
    return 0;
}

void bme680_task (void *arg) {
    uint8_t attempts = 0;
    struct TaskParams *tparams (struct TaskParams *) arg;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    
    while(1) {
        attempts = read_bme680() {
        }
    }
}



const sensor_driver_t bme680_driver = {
    .name     = "BME680",
    .data_len = BME680_DATA_LEN,
    .init     = bme680_init,
    .read     = bme680_read,
};
