/**
 * @file gps.c
 * @brief GPS UART driver — reads NMEA sentences in a background task.
 *
 * A FreeRTOS task continuously reads from the GPS UART and stores the
 * latest complete NMEA sentence.  The main sensor-read loop can poll
 * gps_read() to grab the most recent line.
 */

#include "gps.h"
#include "sensor_config.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "gps";

#define UART_BUF_SIZE   1024

/* Double-buffered latest NMEA sentence */
static uint8_t  s_sentence[GPS_MAX_SENTENCE_LEN];
static size_t   s_sentence_len = 0;
static volatile bool s_new_data = false;

/* ── UART background task ─────────────────────────────────── */

static void gps_rx_task(void *arg)
{
    uint8_t byte;
    uint8_t line_buf[GPS_MAX_SENTENCE_LEN];
    size_t  line_pos = 0;

    ESP_LOGI(TAG, "GPS RX task running");

    while (1) {
        int len = uart_read_bytes(GPS_UART_NUM, &byte, 1, pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        if (byte == '\n' || byte == '\r') {
            if (line_pos > 0) {
                /* Complete sentence — copy to shared buffer */
                memcpy(s_sentence, line_buf, line_pos);
                s_sentence_len = line_pos;
                s_new_data     = true;
                line_pos       = 0;
            }
        } else {
            if (line_pos < GPS_MAX_SENTENCE_LEN - 1) {
                line_buf[line_pos++] = byte;
            }
        }
    }
}

/* ── Public API ───────────────────────────────────────────── */

esp_err_t gps_init(i2c_master_bus_handle_t bus)
{
    (void)bus;  /* GPS doesn't use I2C */

    const uart_config_t uart_cfg = {
        .baud_rate  = GPS_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(GPS_UART_NUM, &uart_cfg);
    if (ret != ESP_OK) return ret;

    ret = uart_set_pin(GPS_UART_NUM, PIN_GPS_TX, PIN_GPS_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    ret = uart_driver_install(GPS_UART_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "GPS UART%d ready  TX=%d RX=%d  %d baud",
             GPS_UART_NUM, PIN_GPS_TX, PIN_GPS_RX, GPS_BAUD_RATE);
    return ESP_OK;
}

void gps_start_task(void)
{
    xTaskCreate(gps_rx_task, "gps_rx", 4096, NULL, 5, NULL);
}

esp_err_t gps_read(uint8_t *out_data)
{
    if (!s_new_data) {
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(out_data, s_sentence, s_sentence_len);
    /* Zero-terminate for convenience (if caller treats it as string) */
    out_data[s_sentence_len] = '\0';
    s_new_data = false;
    return ESP_OK;
}

const sensor_driver_t gps_driver = {
    .name     = "GPS",
    .data_len = GPS_MAX_SENTENCE_LEN,
    .init     = gps_init,
    .read     = gps_read,
};
