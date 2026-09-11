/**
 * @file telemetry_net.h
 * @brief Wi-Fi SoftAP + UDP broadcast transport for the live telemetry stream.
 *
 * Mirrors the exact byte stream the SD logger writes out over the air, so any
 * client that joins the payload's own access point receives the identical
 * framed packets the ground tools already understand (SD-Parser/frameparser.py,
 * and the ESP32-P4 display).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Bring up the SoftAP, open the broadcast socket and start the drain task.
 *
 * Safe to call when the feature is compiled out (returns false, does nothing).
 * Never aborts and never ESP_ERROR_CHECKs: a radio that fails to start must not
 * take the flight computer down with it — the payload keeps flying and logging
 * to SD regardless.
 *
 * @return true if the link is up and broadcasting.
 */
bool telemetry_net_init(void);

/**
 * Queue @p len bytes for broadcast.
 *
 * Returns immediately. If the backlog is full or the lock is momentarily
 * contended the whole chunk is dropped and counted — the SD card is the
 * authoritative recorder and must never wait on the network. A dropped chunk
 * costs the receiver at most one frame, since every frame starts with the
 * 0xAA 0xAA 0xAA sync word and carries a CRC, so the decoder resynchronises
 * on its own.
 *
 * Safe to call from any task.
 */
void telemetry_net_send(const uint8_t *data, size_t len);

/** True when at least one client is associated with the payload's AP. */
bool telemetry_net_has_client(void);

/** Chunks dropped because the link could not keep up (diagnostics only). */
uint32_t telemetry_net_dropped(void);
