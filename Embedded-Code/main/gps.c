
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UART_PC_NUM      UART_NUM_0  // USB serial to PC (u-center)
#define UART_GPS_NUM     UART_NUM_2  // GPS UART
#define BUF_SIZE         1024
#define GPS_TXD_PIN      (16)
#define GPS_RXD_PIN      (17)
#define BLUE_LED_PIN     (2)          // D2 (GPIO2) for blue LED

#define TAG             "GPS"

void blink_led()
{
    gpio_set_level(BLUE_LED_PIN, 1);
    vTaskDelay(50 / portTICK_PERIOD_MS);
    gpio_set_level(BLUE_LED_PIN, 0);
}


void uart_init()
{
    // Suppress all ESP-IDF log output so u-center only sees raw GPS data
    esp_log_level_set("*", ESP_LOG_NONE);

    // Configure UART0 (PC / u-center) — take over from console
    const uart_config_t pc_uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_PC_NUM, &pc_uart_config);
    uart_driver_install(UART_PC_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0);

    // Configure UART2 (GPS)
    const uart_config_t gps_uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_GPS_NUM, &gps_uart_config);
    uart_set_pin(UART_GPS_NUM, GPS_TXD_PIN, GPS_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_GPS_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);

    // Configure blue LED GPIO
    gpio_set_direction(BLUE_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(BLUE_LED_PIN, 0);
    blink_led();
}

// Forward GPS -> PC (u-center receives NMEA/UBX from GPS)
void gps_to_pc_task(void *arg)
{
    uint8_t data[BUF_SIZE];
    while (1) {
        int len = uart_read_bytes(UART_GPS_NUM, data, BUF_SIZE, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            uart_write_bytes(UART_PC_NUM, (const char *)data, len);
            blink_led();
        }
    }
}

// Forward PC -> GPS (u-center sends UBX commands to GPS)
void pc_to_gps_task(void *arg)
{
    uint8_t data[BUF_SIZE];
    while (1) {
        int len = uart_read_bytes(UART_PC_NUM, data, BUF_SIZE, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            uart_write_bytes(UART_GPS_NUM, (const char *)data, len);
        }
    }
}

void app_main(void)
{
    uart_init();

    // Run GPS->PC and PC->GPS as separate FreeRTOS tasks
    xTaskCreate(gps_to_pc_task, "gps_to_pc", 4096, NULL, 10, NULL);
    xTaskCreate(pc_to_gps_task, "pc_to_gps", 4096, NULL, 10, NULL);
}
