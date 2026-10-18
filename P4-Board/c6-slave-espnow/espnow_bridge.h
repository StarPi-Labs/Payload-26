/**
 * @file espnow_bridge.h
 * @brief ESP-NOW receiver bolted onto the esp-hosted co-processor firmware.
 *
 * Add-on for the ESP32-C6 co-processor of the ESP32-P4-Function-EV-Board. It
 * ADDS ESP-NOW reception to esp-hosted's co-processor without altering any of
 * it: Wi-Fi and the whole esp-hosted RPC/data path keep working as before.
 *
 * Received ESP-NOW payloads are forwarded to the P4 host over esp-hosted's
 * peer-data channel (esp-hosted 3.x: eh_cp_feat_peer_data_send), a
 * general-purpose user-data pipe the component already provides - no protobuf
 * changes, no new RPCs.
 *
 * Built by the co-processor project in ./cp - see README.md in this folder.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialise ESP-NOW and start forwarding to the host.
 *
 * ESP-NOW requires a started Wi-Fi stack. esp-hosted 3.x only starts Wi-Fi
 * when the host asks, so if it is not running yet this initialises it in
 * station mode itself. Needs the default event loop to exist.
 *
 * Never aborts. On failure it logs and returns false, leaving the
 * co-processor's normal duties completely unaffected.
 *
 * @return true if ESP-NOW is up and the forwarding task is running.
 */
bool espnow_bridge_init(void);

/** Packets received from the rocket. */
uint32_t espnow_bridge_received(void);

/** Packets dropped because the forward queue was full. */
uint32_t espnow_bridge_dropped(void);
