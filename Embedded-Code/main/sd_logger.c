/**
 * @file sd_logger.c
 * @brief SD card logger — SDMMC 1-bit mode.
 */

#include "sd_logger.h"
#include "sensor_config.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "sd_logger";

#define MOUNT_POINT     "/sdcard"
#define DATA_FILE       MOUNT_POINT "/sensor_data.csv"

static sdmmc_card_t *s_card = NULL;
static FILE         *s_file = NULL;

esp_err_t sd_logger_init(const char *csv_header)
{
    ESP_LOGI(TAG, "Mounting SD card ...");

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;  /* 1-bit mode */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0  = PIN_SD_D0;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    sdmmc_card_print_info(stdout, s_card);

    /* Open / create file */
    struct stat st;
    bool file_exists = (stat(DATA_FILE, &st) == 0);

    s_file = fopen(DATA_FILE, "a");
    if (s_file == NULL) {
        ESP_LOGE(TAG, "fopen failed");
        return ESP_FAIL;
    }

    if (!file_exists && csv_header != NULL) {
        fprintf(s_file, "%s\n", csv_header);
        fflush(s_file);
        ESP_LOGI(TAG, "Created %s with header", DATA_FILE);
    } else {
        ESP_LOGI(TAG, "Appending to %s", DATA_FILE);
    }

    return ESP_OK;
}

FILE *sd_logger_get_file(void)
{
    return s_file;
}

void sd_logger_flush(void)
{
    if (s_file) fflush(s_file);
}

void sd_logger_deinit(void)
{
    if (s_file) {
        fflush(s_file);
        fclose(s_file);
        s_file = NULL;
    }
    if (s_card) {
        // esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_card = NULL;
    }
    ESP_LOGI(TAG, "SD card unmounted");
}
