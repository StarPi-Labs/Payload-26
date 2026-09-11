/**
 * @file espnow_bridge.h
 * @brief ESP-NOW receiver bolted onto the esp-hosted slave firmware.
 *
 * Drop-in add-on for the ESP32-C6 co-processor of the ESP32-P4-Function-EV-Board.
 * It ADDS ESP-NOW reception to the stock slave firmware without altering any of
 * it: Wi-Fi, Bluetooth and the whole esp-hosted RPC/data path keep working
 * exactly as before.
 *
 * Received ESP-NOW payloads are forwarded to the P4 host over esp-hosted's
 * documented custom-data channel, which is a general-purpose user-data pipe the
 * component already provides - no protobuf changes, no new RPCs.
 *
 * See README.md in this folder for how to graft it into the slave project.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialise ESP-NOW and start forwarding to the host.
 *
 * Call AFTER the slave firmware has brought Wi-Fi up (esp_wifi_start has run);
 * ESP-NOW requires a started Wi-Fi stack. In practice that means calling it at
 * the end of the slave's own app_main.
 *
 * Never aborts. On failure it logs and returns false, leaving the slave's
 * normal Wi-Fi/Bluetooth duties completely unaffected.
 *
 * @return true if ESP-NOW is up and the forwarding task is running.
 */
bool espnow_bridge_init(void);

/** Packets received from the rocket. */
uint32_t espnow_bridge_received(void);

/** Packets dropped because the forward queue was full. */
uint32_t espnow_bridge_dropped(void);
