/**
 * @file pilog_link.h
 * @brief Shared constants for the Pi-LOG wireless link.
 *
 * One copy of this file is the source of truth for three separate firmwares:
 *
 *   rocket ESP32-S3   Embedded-Code/main/telemetry_espnow.c   (ESP-NOW sender)
 *   ground ESP32-C6   P4-Board/c6-slave-espnow/espnow_bridge.c (ESP-NOW -> host)
 *   ground ESP32-P4   P4-Board/p4-display/main/main.c          (host receiver)
 *
 * Keep them in sync. A channel or message-ID mismatch produces a link that
 * looks perfectly healthy from both ends and delivers nothing.
 */
#pragma once

#include <stdint.h>

/**
 * Wi-Fi channel used by ESP-NOW.
 *
 * Every participant must agree. Note the coexistence rule: ESP-NOW shares the
 * one radio with the Wi-Fi station, so if the C6 ever associates with an access
 * point, the AP's channel wins and the rocket must follow it. While the ground
 * station is standalone (the normal case for a live flight display) this fixed
 * channel is what everyone uses.
 */
#define PILOG_ESPNOW_CHANNEL    6

/**
 * Largest ESP-NOW payload we will send.
 *
 * ESP_NOW_MAX_DATA_LEN is 250; the margin leaves room for the sequence header
 * below without pushing a chunk over the limit.
 */
#define PILOG_ESPNOW_CHUNK      230

/**
 * esp-hosted custom-data message ID carrying telemetry from the C6 up to the P4.
 *
 * Any uint32_t except 0xFFFFFFFF is legal. This one is arbitrary but must match
 * on both sides of the SDIO link.
 */
#define PILOG_MSGID_TELEMETRY   0x50494C47u   /* "PILG" */

/**
 * Per-chunk header prepended to each ESP-NOW payload.
 *
 * The telemetry stream is a continuous run of framed packets that gets sliced
 * at arbitrary byte boundaries, so chunks are not self-describing. The sequence
 * number lets the receiver notice a gap and report it; it deliberately does NOT
 * try to repair one. Every frame carries its own sync word and CRC, so a lost
 * chunk costs at most the frames it straddled and the decoder resynchronises by
 * itself.
 */
typedef struct __attribute__((packed)) {
    uint32_t seq;        /**< increments per chunk sent */
    uint32_t uptime_ms;  /**< sender uptime, for coarse link-latency sanity */
} pilog_chunk_hdr_t;
