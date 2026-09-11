/**
 * @file telemetry_espnow.h
 * @brief ESP-NOW transport for the live telemetry stream.
 *
 * Broadcasts the exact byte stream the SD logger writes, so the ground station
 * receives the identical framed packets the existing tools already decode.
 *
 * Sibling of telemetry_net.c (Wi-Fi SoftAP + UDP). Both may be enabled at once;
 * both tee off the same funnel in frame_logger.c. ESP-NOW is connectionless and
 * needs no association, so it starts delivering the instant both ends are
 * powered - which is why it is the better fit for a payload that gets sealed
 * into a rocket.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Bring up ESP-NOW and start the drain task.
 *
 * Safe to call when compiled out (returns false, does nothing). Never aborts:
 * a radio that refuses to start must not take the flight computer down with it.
 *
 * @return true if the link is up and broadcasting.
 */
bool telemetry_espnow_init(void);

/**
 * Queue @p len bytes for broadcast. Returns immediately.
 *
 * Drops whole chunks when the backlog is full or the lock is contended - the
 * SD card is the authoritative recorder and must never wait on the radio.
 * Safe to call from any task.
 */
void telemetry_espnow_send(const uint8_t *data, size_t len);

/** Chunks dropped because the link could not keep up (diagnostics only). */
uint32_t telemetry_espnow_dropped(void);

/** Chunks successfully handed to the radio. */
uint32_t telemetry_espnow_sent(void);
