/**
 * @file i2c_bus.c
 * @brief Shared I2C master bus mentation.
 */

#include "i2c_bus.h"
#include "sensor_config.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "i2c_bus";

#define I2C_TIMEOUT_MS  10

static i2c_master_bus_handle_t s_bus0_handle = NULL;
static i2c_master_bus_handle_t s_bus1_handle = NULL;

void i2c_force_bus_reset(gpio_num_t sda_pin, gpio_num_t scl_pin) {
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pin_bit_mask = (1ULL << sda_pin) | (1ULL << scl_pin),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);

    /**
     * Let-me-go! yells the bus
     *  Bit-bang SCL 9 times: 8 bits + ack would be enough
     */
    for (int i = 0; i < 9; i ++) {
        gpio_set_level(scl_pin, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl_pin, 1);
        esp_rom_delay_us(5);

        /* if SDA is high, then bus may be free. */
        if (gpio_get_level(sda_pin) == 1) break;
    }

    /**
     * Manual STOP of the bus
     *  SDA goes Low-High while SCL is high.
     */
    gpio_set_level(sda_pin, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl_pin, 1);
    esp_rom_delay_us(5);
    gpio_set_level(sda_pin, 1);

}

esp_err_t 
i2c_bus_init(
    i2c_port_num_t i2c_port, 
    gpio_num_t sda_pin, 
    gpio_num_t scl_pin, 
    i2c_master_bus_handle_t *handler
)
{
    if (*handler != NULL) {
        ESP_LOGW(TAG, "I2C bus already initialised");
        return ESP_OK;
    }

    /**
     * To ensure reliability:
     * - bit-bang the SCL (in case it got stuck on previous failure).
     * - Manual I2C stop bus
     */
    i2c_force_bus_reset(sda_pin, scl_pin);
    
    /**
     * Bus ready for configuration 
     *  Over-writing is ok :)
     */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = i2c_port,
        .sda_io_num        = sda_pin,
        .scl_io_num        = scl_pin,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 14,
        .flags.enable_internal_pullup = false,
    };
        //.flags.enable_internal_pullup = true,

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, handler);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "I2C bus ready  SDA=%d  SCL=%d  %d Hz",
                 sda_pin, scl_pin, CONFIG_I2C_MASTER_FREQUENCY);
        return ret;
    }

    ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t 
sys_i2c_init(
    i2c_port_num_t i2c_port, 
    gpio_num_t sda_pin, 
    gpio_num_t scl_pin
) {
    i2c_master_bus_handle_t *tmp_handle;
    switch (i2c_port) {
    case I2C_NUM_0:
        tmp_handle = &s_bus0_handle; break;
    case I2C_NUM_1:
        tmp_handle = &s_bus1_handle; break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_bus_init(i2c_port, 
                sda_pin, 
                scl_pin, 
                tmp_handle);
 
}

i2c_master_bus_handle_t i2c_bus0_get_handle(void) {
    return s_bus0_handle;
}

i2c_master_bus_handle_t i2c_bus1_get_handle(void) {
    return s_bus1_handle;
}

esp_err_t 
i2c_bind (
    i2c_master_bus_handle_t bus, 
    i2c_master_dev_handle_t *dev,
    uint16_t device_addr
) 
{
    if (NULL == bus) {
        ESP_LOGE(TAG, "Something went wrong with the bus");
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = device_addr,
        .scl_speed_hz = I2C_MASTER_CLK,
    };
        //.scl_speed_hz = I2C_MASTER_CLK,
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, dev);
    return err;
}



esp_err_t 
i2c_bus_write_byte (
    i2c_master_dev_handle_t dev, 
    uint8_t reg, 
    uint8_t val
    )
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t 
i2c_bus_read_bytes(
    i2c_master_dev_handle_t dev, 
    uint8_t reg, 
    uint8_t *buf, 
    size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}
