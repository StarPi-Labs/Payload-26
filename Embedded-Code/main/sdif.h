#ifndef _SD_INTERFACE_H_
#define _SD_INTERFACE_H_

#ifdef CONFIG_ENABLE_SD_SPI
#include "driver/spi_master.h"

esp_err_t sys_hardware_sd_init(spi_host_device_t spi_port);
esp_err_t sys_sd_init(spi_host_device_t spi_port, sdmmc_card_t *card, size_t *starting_sector);
esp_err_t sys_mount_spi_card(spi_host_device_t spi_port, const char* mount_point, sdmmc_card_t **card);

esp_err_t spi_sd_pre_erase(sdmmc_card_t *card, uint32_t num_sectors);
void sys_fs_unmount(sdmmc_card_t *card);

#elif CONFIG_ENABLE_SD_SDIO 

#endif

#endif
