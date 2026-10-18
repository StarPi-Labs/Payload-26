/**
 * @file telemetry_espnow.h
 * @brief ESP-NOW transport for the live telemetry stream.
 *
 * Broadcasts the same framed byte stream the SD logger records, so the ground
 * station decodes it with the existing tools. Unlike the SD, which only gets a
 * sparse slice while ARMED, the radio is fed every frame live in every mode,
 * plus a calibration frame every few seconds (radio_forward in frame_logger.c).
 *
 * Sibling of telemetry_net.c (Wi-Fi SoftAP + UDP). Both may be enabled at once;
 * both are fed by radio_forward() in frame_logger.c. ESP-NOW is connectionless and
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

/**
 * Request the link to switch off for good (used at launch). Returns at once.
 *
 * Only sets a flag. The drain task - low priority, never a flight-critical
 * one - then deinitialises ESP-NOW and, when it is the radio's only user,
 * stops and deinitialises Wi-Fi, and exits. Queued but unsent bytes are
 * dropped; sends after this call are ignored. Idempotent, safe from any task.
 */
void telemetry_espnow_shutdown(void);

/** Chunks dropped because the link could not keep up (diagnostics only). */
uint32_t telemetry_espnow_dropped(void);

/** Chunks successfully handed to the radio. */
uint32_t telemetry_espnow_sent(void);
