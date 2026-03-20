/**
 * @file uart.h
 * @brief uart util functions 
 */

#pragma once

#include "esp_err.h"

typedef struct {
    uart_port_t port;
    int boudrate;
    int tx_pin;
    int rx_pin;
    int rx_buffer_sz;
} small_uart_cfg_t;

esp_err_t sys_uart_init(
    small_uart_cfg_t ucfg
);
