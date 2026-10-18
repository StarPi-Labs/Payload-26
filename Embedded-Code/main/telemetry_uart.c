/**
 * @file telemetry_uart.c
 * @brief Wired telemetry over UART0 TX - see telemetry_uart.h.
 *
 * Formerly bt_serial_bridge.c. The ESP32-S3 has no Classic Bluetooth, so its
 * SPP branch never ran on this board; Bluetooth has been removed and this is
 * the transport that was actually in use.
 */

#include "telemetry_uart.h"

#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "telem_uart";

/* DevKitC port mapping: the console lives on USB-Serial-JTAG (the "USB"
 * connector), which frees UART0 and its native pins GPIO43/44 - hardwired to
 * the on-board CP210x bridge (the "UART" connector). Telemetry therefore
 * comes out of the "UART" USB port with NO external wiring, and the two
 * streams can never mix. (sdkconfig: ESP_CONSOLE_USB_SERIAL_JTAG.) */
#define TELEM_UART      UART_NUM_0
#define TELEM_TX_PIN    43
#define TELEM_BAUD      115200

static bool s_started = false;

bool telemetry_uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = TELEM_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(TELEM_UART, 256, 4096, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "telemetry UART install failed");
        return false;
    }
    uart_param_config(TELEM_UART, &cfg);
    uart_set_pin(TELEM_UART, TELEM_TX_PIN, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    s_started = true;
    ESP_LOGI(TAG, "telemetry on UART%d TX=GPIO%d @ %d baud",
             TELEM_UART, TELEM_TX_PIN, TELEM_BAUD);
    return true;
}

void telemetry_uart_write(const uint8_t *data, size_t len)
{
    if (s_started && data && len) uart_write_bytes(TELEM_UART, (const char *)data, len);
}
