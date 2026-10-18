/**
 * @file telemetry_uart.h
 * @brief Wired telemetry: the framed byte stream out of UART0 (TX only).
 *
 * On the ESP32-S3 DevKitC, UART0's native pins GPIO43/44 are hardwired to the
 * on-board CP210x, so the stream comes out of the "UART" USB connector with no
 * extra wiring, while the console stays on USB-Serial-JTAG (the "USB"
 * connector). The host reads it with frameparser/bin2json.
 *
 * Used for the pad health telemetry and, with no SD card, for mirroring the
 * exact stream the card would have received.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Install the UART driver. Returns false (and logs) if it cannot. */
bool telemetry_uart_init(void);

/** Queue bytes for transmission. No-op until telemetry_uart_init() succeeded. */
void telemetry_uart_write(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
