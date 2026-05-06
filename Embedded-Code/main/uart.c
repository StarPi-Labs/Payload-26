/**
 * @file uart.c
 * @brief uart util functions
 */

#include "esp_log.h"
#include "uart.h"

static const char *TAG = "UART";

esp_err_t sys_uart_baud(uart_port_t port, int baud) {
    return uart_set_baudrate(port, baud);
}

void sys_uart_flush(uart_port_t port) {
    uart_flush_input(port);
}

esp_err_t 
sys_uart_init (
    uart_port_t port, 
    int baud, 
    gpio_num_t tx_pin, 
    gpio_num_t rx_pin, 
    int tx_buf_sz, 
    int rx_buf_sz
) 
{
    const uart_config_t uart_cfg = {
        .baud_rate  = baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(port, &uart_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Setting params");
        return ret;
    }

    ret = uart_set_pin(
        port, 
        tx_pin,
        rx_pin,
        UART_PIN_NO_CHANGE, 
        UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error setting pins.");
        return ret;
    }

    ret = uart_driver_install(
        port, 
        rx_buf_sz * 2, 
        tx_buf_sz, 
        0, 
        NULL, 
        0);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error installing driver: %s.", esp_err_to_name(ret));
    }
    return ret;
}
