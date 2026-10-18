/**
 * @file sd_backup.h
 * @brief Ground-side backup of the received telemetry on the P4's microSD.
 *
 * Every byte that arrives from the rocket is appended, unmodified, to a new
 * file per boot - /sdcard/PILOG/RX_nnnn.BIN - in exactly the format the
 * payload's own SD card uses, so SD-Parser's bin2json.py and the rest of the
 * ground tools read it directly.
 *
 * It only holds what made it over the air: the pad phase, up to the launch
 * cut-off. The payload's own card stays the flight record.
 *
 * The card shares the P4's SDMMC controller with the esp-hosted link to the
 * C6 (card on slot 0, C6 on slot 1). Mount it only after the esp-hosted host
 * is up, which is what owns the controller.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool     active;        /**< card mounted and file open */
    uint32_t bytes;         /**< bytes written so far */
    uint32_t dropped;       /**< bytes lost because the card fell behind */
    char     name[24];      /**< e.g. "RX_0007.BIN" */
} sd_backup_info_t;

/**
 * Mount the card and open the next free RX_nnnn.BIN.
 *
 * @param controller_ready  true when esp-hosted already initialised the SDMMC
 *                          controller (the normal case); false makes this
 *                          initialise it itself.
 * @return true if recording; false (logged) with no card or on any failure.
 */
bool sd_backup_start(bool controller_ready);

/** Queue received bytes. Never blocks; drops (and counts) if the card lags. */
void sd_backup_write(const uint8_t *data, size_t len);

/** Snapshot of the backup state for display. */
void sd_backup_info(sd_backup_info_t *out);
