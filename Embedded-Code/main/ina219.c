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

static const char *TAG = "ina219";

/* I2C address (Adafruit default with no jumpers) */
#define INA219_ADDR             0x40

/* Register addresses */
#define REG_CONFIG              0x00
#define REG_SHUNT_VOLTAGE       0x01
#define REG_BUS_VOLTAGE         0x02
#define REG_POWER               0x03
#define REG_CURRENT             0x04
#define REG_CALIBRATION         0x05

/* Configuration register bits */
#define CONFIG_RESET            (1 << 15)
#define CONFIG_BRNG_32V         (1 << 13)   /* Bus voltage range 32V */
#define CONFIG_PGA_320MV        (0x3 << 11) /* Shunt voltage range ±320mV */
#define CONFIG_BADC_12BIT       (0x3 << 7)  /* Bus ADC 12-bit */
#define CONFIG_SADC_12BIT       (0x3 << 3)  /* Shunt ADC 12-bit */
#define CONFIG_MODE_CONTINUOUS  0x07        /* Shunt + bus, continuous */

/* Default config: 32V, 320mV, 12-bit, continuous */
#define INA219_CONFIG_DEFAULT   (CONFIG_BRNG_32V | CONFIG_PGA_320MV | \
                                 CONFIG_BADC_12BIT | CONFIG_SADC_12BIT | \
                                 CONFIG_MODE_CONTINUOUS)

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

esp_err_t ina219_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = INA219_ADDR,
        .scl_speed_hz    = I2C_MASTER_FREQ_HZ,
    };
    
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add INA219: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Reset the device */
    ret = ina219_write_reg16(REG_CONFIG, CONFIG_RESET);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Reset failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(1)); /* Wait for reset */

    /* Apply configuration */
    ret = ina219_write_reg16(REG_CONFIG, INA219_CONFIG_DEFAULT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set calibration register for current/power readings */
    ret = ina219_write_reg16(REG_CALIBRATION, INA219_CALIBRATION);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Calibration write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Verify calibration was written */
    uint16_t cal_readback = 0;
    ret = ina219_read_reg16(REG_CALIBRATION, &cal_readback);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Calibration = 0x%04X (expected 0x%04X)", 
                 cal_readback, INA219_CALIBRATION);
    }

    ESP_LOGI(TAG, "INA219 initialised @ 0x%02X (0.1Ω shunt, ±3.2A range)", INA219_ADDR);
    return ESP_OK;
}

esp_err_t ina219_read(uint8_t *out_data)
{
    uint16_t shunt, bus, power, current;
    esp_err_t ret;

    /* Read all four measurement registers */
    ret = ina219_read_reg16(REG_SHUNT_VOLTAGE, &shunt);
    if (ret != ESP_OK) return ret;

    ret = ina219_read_reg16(REG_BUS_VOLTAGE, &bus);
    if (ret != ESP_OK) return ret;

    ret = ina219_read_reg16(REG_POWER, &power);
    if (ret != ESP_OK) return ret;

    ret = ina219_read_reg16(REG_CURRENT, &current);
    if (ret != ESP_OK) return ret;

    /* Pack into output buffer (big-endian) */
    out_data[0] = (uint8_t)(shunt >> 8);
    out_data[1] = (uint8_t)(shunt & 0xFF);
    out_data[2] = (uint8_t)(bus >> 8);
    out_data[3] = (uint8_t)(bus & 0xFF);
    out_data[4] = (uint8_t)(power >> 8);
    out_data[5] = (uint8_t)(power & 0xFF);
    out_data[6] = (uint8_t)(current >> 8);
    out_data[7] = (uint8_t)(current & 0xFF);

    return ESP_OK;
}

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

const sensor_driver_t ina219_driver = {
    .name     = "INA219",
    .data_len = INA219_DATA_LEN,
    .init     = ina219_init,
    .read     = ina219_read,
};
