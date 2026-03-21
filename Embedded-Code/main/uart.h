/**
 * @file uart.h
 * @brief uart util functions 
 */

#pragma once

#include "esp_err.h"
#include "driver/uart.h"
#include "driver/gpio.h"

esp_err_t 
sys_uart_init (
    uart_port_t port, 
    int baud, 
    gpio_num_t tx_pin, 
    gpio_num_t rx_pin, 
    int tx_buf_sz, 
    int rx_buf_sz
);
