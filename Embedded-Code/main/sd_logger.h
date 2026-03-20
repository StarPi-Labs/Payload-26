/**
 * @file sd_logger.h
 * @brief SD card logger — mounts card, opens CSV file, provides write helpers.
 */

#pragma once

#include "esp_err.h"
#include <stdio.h>

/**
 * Mount the SD card (SDMMC 1-bit mode) and open/create the CSV file.
 * @param csv_header  CSV header line to write if the file is new (without trailing newline).
 * @return ESP_OK on success.
 */
esp_err_t sd_logger_init(const char *csv_header);
esp_err_t sd_card_init();

/**
 * Get the open file handle for direct fprintf() calls.
 * Returns NULL if SD is not mounted.
 */
FILE *sd_logger_get_file(void);

/**
 * Flush the file to disk. Call periodically.
 */
void sd_logger_flush(void);

/**
 * Close file and unmount SD card.
 */
void sd_logger_deinit(void);
