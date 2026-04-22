#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sdif.h"

#define SD_MOUNTING_POINT   "/sd"

#ifdef CONFIG_ENABLE_SD_SPI

esp_err_t sys_hardware_sd_init(spi_host_device_t spi_port) {
    esp_err_t ret;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = GPIO_NUM_13,
        .miso_io_num = GPIO_NUM_12,
        .sclk_io_num = GPIO_NUM_14, // 14
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8192
    };
    ret = spi_bus_initialize(spi_port,&bus_cfg, SPI_DMA_CH_AUTO);

    return ret;
}

void sys_fs_unmount(sdmmc_card_t *card) {
    ESP_LOGI("SD", "Unmounting");
    // esp_vfs_fat_sdcard_unmount(SD_MOUNTING_POINT, card);
}

esp_err_t sys_fs_mount(spi_host_device_t spi_port, sdmmc_card_t **card) {
    esp_log_level_set("sdmmc_init", ESP_LOG_DEBUG);
    esp_log_level_set("sdspi_host", ESP_LOG_DEBUG);
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 1,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = spi_port;
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = GPIO_NUM_15;
    slot_cfg.host_id = host.slot;

    return esp_vfs_fat_sdspi_mount(SD_MOUNTING_POINT, &host, &slot_cfg, &mount_cfg, card);
}


#elif SDIO
esp_err_t SD_HW_init(void) {
    return ESP_OK;
}


#endif
