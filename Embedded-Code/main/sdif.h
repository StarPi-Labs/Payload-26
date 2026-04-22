#ifndef _SD_INTERFACE_H_
#define _SD_INTERFACE_H_

#ifdef CONFIG_ENABLE_SD_SPI
#include "driver/spi_master.h"

esp_err_t sys_hardware_sd_init(spi_host_device_t spi_port);
esp_err_t sys_fs_mount(spi_host_device_t spi_port, sdmmc_card_t **card);
void sys_fs_unmount(sdmmc_card_t *card);

#elif CONFIG_ENABLE_SD_SDIO 

#endif

#endif
