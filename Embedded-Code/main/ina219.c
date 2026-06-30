/**
 * @file ina219.c
 * @brief INA219 current / voltage / power monitor driver.
 *
 * Default configuration (Adafruit breakout compatible):
 *   - Bus voltage range: 32V (BRNG = 1)
 *   - Shunt voltage range: ±320mV (PGA = /8)
 *   - ADC resolution: 12-bit, 1 sample
 *   - Mode: Shunt and bus, continuous
 *
 * With 0.1Ω shunt (Adafruit default):
 *   - Max current: 3.2A
 *   - Current LSB: ~0.1mA (after calibration)
 */

#include "ina219.h"
#include "i2c_bus.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "systemp2i.h"
#include "flight_stats.h"

static const char *TAG = "ina219";

/* I2C address (Adafruit default with no jumpers) */
#define INA219_ADDR             0x40

/* Register addresses */
#define INA219_REG_CONFIG       0x00
#define INA219_REG_SHUNT_VOLT   0x01
#define INA219_REG_BUS_VOLT     0x02
#define INA219_REG_POWER        0x03
#define INA219_REG_CURRENT      0x04
#define INA219_REG_CALIBRATION  0x05

#define INA219_CONFIG_ON_RESET  0x399F
#define INA219_SW_RESET         0x8000

/* Configuration register bits */
#define INA219_BRNG_16v         (0 << 13)   // Max./min. Bus voltage +/-16v 
#define INA219_PGA_80mV         (0x3 << 11) // Max./min. Shunt voltage +/-80mV, 
                                            // Max./min. Current@0.1Omh 0.8A
#define INA219_BADC_12BIT       (0x3 << 7)  // Bus ADC 12-bit, Sampling Period: 532us
#define INA219_SADC_12BIT       (0x3 << 3)  // Shunt ADC 12-bit, Sampling Period: 532us
#define INA219_MODE_CONTINUOUS  0x07        // Shunt + bus, continuous 

/* Default config: 16V, 80mV, 12-bit, continuous */
#define INA219_SYSTEMp2i_CONFIG (INA219_BRNG_16v | \
                                 INA219_PGA_80mV | \
                                 INA219_BADC_12BIT | \
                                 INA219_SADC_12BIT | \
                                 INA219_MODE_CONTINUOUS)

#define INA219_MAX_FAILED_ATTEMPTS  60

/*
 * Calibration for 0.1Ω shunt resistor (Adafruit INA219 breakout):
 *   Cal = trunc(0.04096 / (Current_LSB * R_shunt))
 *   With Current_LSB = 0.1mA = 0.0001A, R_shunt = 0.1Ω:
 *   Cal = trunc(0.04096 / (0.0001 * 0.1)) = 4096 = 0x1000
 *
 * Current_LSB = 0.1mA means the current register value × 0.1 = mA
 * Power_LSB = 20 × Current_LSB = 2mW
 */
#define INA219_CALIBRATION      0x1000
#define CURRENT_LSB_UA          100     /* 0.1 mA = 100 µA per LSB */
#define POWER_LSB_UW            2000    /* 2 mW = 2000 µW per LSB */

/* Shunt voltage: 10 µV per LSB (from datasheet) */
#define SHUNT_LSB_UV            10

/* Bus voltage: 4 mV per LSB (from datasheet), bits [15:3] */
#define BUS_LSB_MV              4
#define INA219_DATA_LEN         8

static i2c_master_dev_handle_t s_dev = NULL;

/**
 * Write a 16-bit register (big-endian on the wire).
 */
static esp_err_t ina219_write_reg16(uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

/**
 * Read a 16-bit register (big-endian on the wire).
 */
static esp_err_t ina219_read_reg16(uint8_t reg, uint16_t *value)
{
    uint8_t buf[2];
    esp_err_t ret = i2c_bus_read_bytes(s_dev, reg, buf, 2);
    if (ret == ESP_OK) {
        *value = ((uint16_t)buf[0] << 8) | buf[1];
    }
    return ret;
}

esp_err_t ina219_init(i2c_master_bus_handle_t bus) {
    esp_err_t ret = i2c_bind(bus, &s_dev, INA219_ADDR);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add INA219: %s", esp_err_to_name(ret));
        return ret;
    }

    /**
     * Funny thing, this device doesn't have an any id register
     * but on POWER-ON the configuration is fixed 0x399F, we are
     * exploiting that, and in case the ESP32 failed due to anything
     * but the INA219 didn't, the we have to force a device reset.
     * (no worries about the bus, if it got stuck, in the configuration
     * bus it got bit-banged)
     */

    /* Ping device { */
    /* 1. Reset the device */
    ret = ina219_write_reg16(INA219_REG_CONFIG, INA219_SW_RESET);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect with sensor, reset failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(1)); /* NOTE: 1 seems to be OK */

    /* 2. Read default configuration */
    uint16_t config_default = 0;
    ret = ina219_read_reg16(INA219_REG_CONFIG, &config_default);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read default configuration: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. Is it the default value though? */
    if (config_default != INA219_CONFIG_ON_RESET) {
        ESP_LOGE(TAG, "Wrong Sensor or device damanged: Expected 0x%04X, got 0x%04X", INA219_CONFIG_ON_RESET, config_default);
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* END Ping } */

    // Configure the Device
    ret = ina219_write_reg16(INA219_REG_CONFIG, INA219_SYSTEMp2i_CONFIG);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ret;
}

/* ── INA219 background task ─────────────────────────────────── */
uint8_t read_shunt_bus_volts(uint8_t *sv, uint8_t *bv, uint8_t attempts) {
    esp_err_t ret = i2c_bus_read_bytes(s_dev, INA219_REG_SHUNT_VOLT, sv, 2);
    if (ret != ESP_OK) {
        attempts++;
        return attempts;
    }
    ret = i2c_bus_read_bytes(s_dev, INA219_REG_BUS_VOLT, bv, 2);
    if (ret != ESP_OK) {
        attempts++;
        return attempts;
    }
    return 0;
}
 
struct __attribute__((packed)) INA219Info {
    int16_t shunt_voltage;
    int16_t bus_voltage;
};
typedef union {
    struct __attribute__((packed)) {
        int16_t shunt_voltage;
        int16_t bus_voltage;
    } values;
    uint32_t raw_all; // This forces 4-byte alignment for the union
} ina_data_t;

void IRAM_ATTR ina219_task(void *arg) {
    uint8_t shunt_volt[2] = {0xC, 0xA}, bus_volt[2] = {0xF, 0xE};
    uint8_t attempts = 0;
    uint16_t telemetry_counter = 0;
    //ina_data_t ina219_info;
    struct INA219Info ina219_info;
    struct TaskParams *tparams = (struct TaskParams *) arg;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    //ina219_info.raw_all = 0;
    // TODO:
    // - we need to protect the bus
    // - we need to pass the pass in the task params along side its mutex to protect 
    //   the bus. Otherwise it may cause problems when appending the BME680

    while(1) {
        attempts = read_shunt_bus_volts(shunt_volt, bus_volt, attempts);

        if (INA219_MAX_FAILED_ATTEMPTS == attempts) {
            // TODO: should I report this event? INA219 down?
            break;
        }

        flight_stats_tick(STAT_INA219);

        ina219_info.shunt_voltage = (int16_t)((shunt_volt[0] << 8) | shunt_volt[1]);
        ina219_info.bus_voltage   = (int16_t)((bus_volt[0] << 8) | bus_volt[1]);
        ina219_info.bus_voltage   = ina219_info.bus_voltage >> 3;

        switch(tparams->context->mode) {
        case MODE_POST:
        case MODE_ARMED:
            telemetry_counter++;
            if (telemetry_counter >= INA219_HM_SKIP_SAMPLES) {
                ESP_LOGI(TAG, "I=%.1f mA  Vbus=%.3f V  (shunt_raw=%d)",
                         ina219_info.shunt_voltage * 0.1f,   /* 10uV/LSB over 0.1ohm = 0.1mA/LSB */
                         ina219_info.bus_voltage * 0.004f,   /* 4mV/LSB (already >>3) */
                         ina219_info.shunt_voltage);
                hm_send(
                    tparams->hm_buffer,
                    SBIT_INA219,
                    (uint8_t *) &ina219_info,
                    sizeof(struct INA219Info)
                );
                telemetry_counter = 0;
            }
            break;
        case MODE_BOOST:
        case MODE_COAST:
            write_to_ring_buffer(
                tparams->log_buffer, 
                SBIT_INA219, 
                (void *) &ina219_info, 
                sizeof(struct INA219Info)
            );

            break;
        }

        memset(&ina219_info, 0, sizeof(struct INA219Info));
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(INA219_PERIOD_default));
    }

    ESP_LOGE(TAG, "INA219 stopped after repeated read failures");
    vTaskDelete(NULL);
}

esp_err_t ina219_config(void)
{
    /*
    // Reset the device 
    esp_err_t ret = ina219_write_reg16(REG_CONFIG, CONFIG_RESET);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Reset failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(1)); // Wait for reset 

    // Apply configuration 
    ret = ina219_write_reg16(REG_CONFIG, INA219_CONFIG_DEFAULT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set calibration register for current/power readings 
    ret = ina219_write_reg16(REG_CALIBRATION, INA219_CALIBRATION);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Calibration write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Verify calibration was written
    uint16_t cal_readback = 0;
    ret = ina219_read_reg16(REG_CALIBRATION, &cal_readback);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Calibration = 0x%04X (expected 0x%04X)", 
                 cal_readback, INA219_CALIBRATION);
    }

    ESP_LOGI(TAG, "INA219 initialised @ 0x%02X (0.1Ω shunt, ±3.2A range)", INA219_ADDR);
    return ESP_OK;
    */
    return ESP_OK;
}

/* -----------------------------------------------
 * THIS SECTION DOESN'T HAVE TO BE RUN AT RUNTIME
 * WE ONLY NEED RAW DATA, THIS CAN BE DONE EITHER
 * ON POST-PROCESSING OR UPON RECEPTION DURING 
 * HEALTH MONITORING.
 * -----------------------------------------------
 */
int32_t ina219_get_shunt_voltage_uv(const uint8_t *data)
{
    /* Shunt voltage is signed 16-bit, LSB = 10 µV */
    int16_t raw = (int16_t)(((uint16_t)data[0] << 8) | data[1]);
    return (int32_t)raw * SHUNT_LSB_UV;
}

int32_t ina219_get_bus_voltage_mv(const uint8_t *data)
{
    /* Bus voltage is in bits [15:3], LSB = 4 mV */
    uint16_t raw = ((uint16_t)data[2] << 8) | data[3];
    return (int32_t)((raw >> 3) * BUS_LSB_MV);
}

int32_t ina219_get_power_uw(const uint8_t *data)
{
    /* Power register, LSB = 2 mW = 2000 µW */
    uint16_t raw = ((uint16_t)data[4] << 8) | data[5];
    return (int32_t)raw * POWER_LSB_UW;
}

int32_t ina219_get_current_ua(const uint8_t *data)
{
    /* Current is signed 16-bit, LSB = 100 µA (0.1 mA) */
    int16_t raw = (int16_t)(((uint16_t)data[6] << 8) | data[7]);
    return (int32_t)raw * CURRENT_LSB_UA;
}
/* -----------------------------------------------
 * END OF POST-PROCESSING SECTION
 * -----------------------------------------------
 */

const sensor_driver_t ina219_driver = {
    .name     = "INA219",
    .data_len = INA219_DATA_LEN,
    .init     = NULL,
    .read     = NULL,
};
