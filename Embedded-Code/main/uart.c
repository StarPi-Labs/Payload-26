/**
 * @file uart.c
 * @brief uart util functions
 */

#include "uart.h"

esp_err_t sys_uart_init(small_uart_cfg_t cfg) {

    const uart_config_t uart_cfg = {
        .baud_rate  = cfg.baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(cfg.port, &uart_cfg);
    if (ret != ESP_OK) return ret;

    ret = uart_set_pin(
        cfg.port, 
        cfg.tx_pin,
        cfg.rx_pin,
        UART_PIN_NO_CHANGE, 
        UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    ret = uart_driver_install(
        cfg.port, 
        cfg.buff_sz * 2, 
        0, 
        0, 
        NULL, 
        0);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}
