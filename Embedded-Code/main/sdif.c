#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sdif.h"
#include "sensor_config.h"
#include <string.h>

#define SD_MOUNTING_POINT   "/sd"

#ifdef CONFIG_ENABLE_SD_SPI

#define SDIF_TAG "SDIF"

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

#define SD_SECTOR_PER_PAGE  64
size_t find_writeable_sector(sdmmc_card_t *card, uint8_t *buffer, size_t low, size_t n) {
    size_t high, mid;
    size_t found_sector = 0;
    low = low / SD_SECTOR_PER_PAGE;
    high = n / SD_SECTOR_PER_PAGE;

    while(low <= high) {
        mid = low + ((high - low) >> 1);
        mid = mid / SD_SECTOR_PER_PAGE;
        if (sdmmc_read_sectors(card, buffer, mid, 1) != ESP_OK) {
            return 0;
        }
        // 16 is a magic number, there's nothing specific about it.
        if (memcmp(buffer, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) != 0) {
            low = mid + 1;
        } else {
            found_sector = mid;
            high = mid - 1;
        }
    }

    if ((0 == found_sector) && (low >= n)) {
        return 0;
    }

    return found_sector;
}

esp_err_t send_raw_cmd(sdmmc_card_t *card, uint32_t opcode, uint32_t arg) {
    sdmmc_command_t cmd = {
        .opcode = opcode,
        .arg = arg,
        .flags = SCF_RSP_R1 | SCF_CMD_AC | SCF_WAIT_BUSY
    };
    // This is the direct driver call
    return (*card->host.do_transaction)(card->host.slot, &cmd);
}

esp_err_t spi_sd_pre_erase(sdmmc_card_t *card, uint32_t num_blocks) {
    // CMD55 first (Required for SPI)
    esp_err_t ret = send_raw_cmd(card, 55, 0);
    if (ret != ESP_OK) return ret;

    // ACMD23
    return send_raw_cmd(card, 23, num_blocks);
}

esp_err_t _erase_sectors(sdmmc_card_t *card, uint32_t start_sector, uint32_t sector_count) {
    uint32_t end_sector = start_sector + sector_count - 1;

    // 1. Set the Start Address for the erase
    printf("Sending cmd32\n");
    sdmmc_command_t cmd32 = {
        .opcode = 32,
        .arg = start_sector,
        .flags = SCF_RSP_R1 | SCF_CMD_AC
    };
    esp_err_t ret = (*card->host.do_transaction)(card->host.slot, &cmd32);
    if (ret != ESP_OK) return ret;

    // 2. Set the End Address for the erase
    printf("sending cmd33\n");
    sdmmc_command_t cmd33 = {
        .opcode = 33,
        .arg = end_sector,
        .flags = SCF_RSP_R1 | SCF_CMD_AC
    };
    ret = (*card->host.do_transaction)(card->host.slot, &cmd33);
    if (ret != ESP_OK) return ret;

    // 3. Execute the Erase
    // This tells the FTL: "Mark everything between 32 and 33 as empty."
    sdmmc_command_t cmd38 = {
        .opcode = 38,
        .arg = 0,
        .flags = SCF_RSP_R1B | SCF_CMD_AC | SCF_WAIT_BUSY, // WAIT_BUSY is critical!
        .timeout_ms = 10000
    };
    printf("Starting Physical Erase of %lu sectors... stay calm.\n", sector_count);
    return (*card->host.do_transaction)(card->host.slot, &cmd38);
}

#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_types.h"

esp_err_t force_cmd38_erase(sdmmc_card_t *card, uint32_t start_sector, uint32_t total_sectors) {
    // 8192 sectors = 4MB. Most cards handle 4MB erases in < 500ms.
    uint32_t chunk_size = 8192;
    uint32_t sectors_done = 0;

    while (sectors_done < total_sectors) {
        uint32_t current_chunk = (total_sectors - sectors_done > chunk_size) ? chunk_size : (total_sectors - sectors_done);
        uint32_t start = start_sector + sectors_done;
        uint32_t end = start + current_chunk - 1;

        // 1. Set Start Address
        sdmmc_command_t cmd32 = {
            .opcode = 32,
            .arg = start,
            .flags = SCF_RSP_R1 | SCF_CMD_AC
        };
        esp_err_t ret = (*card->host.do_transaction)(card->host.slot, &cmd32);
        if (ret != ESP_OK) return ret;

        // 2. Set End Address
        sdmmc_command_t cmd33 = {
            .opcode = 33,
            .arg = end,
            .flags = SCF_RSP_R1 | SCF_CMD_AC
        };
        ret = (*card->host.do_transaction)(card->host.slot, &cmd33);
        if (ret != ESP_OK) return ret;

        // 3. EXECUTE ERASE (The CMD38 you wanted)
        // We use R1B response and wait_busy.
        // Because the chunk is small, it will finish BEFORE the 1s timeout.
        sdmmc_command_t cmd38 = {
            .opcode = 38,
            .arg = 0,
            .flags = SCF_RSP_R1B | SCF_CMD_AC | SCF_WAIT_BUSY
        };

        ret = (*card->host.do_transaction)(card->host.slot, &cmd38);
        if (ret != ESP_OK) {
            printf("CMD38 failed at sector %lu: 0x%x\n", start, ret);
            return ret;
        }

        sectors_done += current_chunk;
        printf("Erased %lu/%lu sectors...\n", sectors_done, total_sectors);

        // Give the card's internal CPU a 10ms breather between chunks
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    printf("CMD38 successful. Your sectors are now physical ghosts.\n");
    return ESP_OK;
}

esp_err_t manual_cmd38_erase(sdmmc_card_t *card, uint32_t start_sector, uint32_t count) {
    uint32_t end_sector = start_sector + count - 1;

    // 1. Set Start Address (CMD32)
    sdmmc_command_t cmd32 = { .opcode = 32, .arg = start_sector, .flags = SCF_RSP_R1 | SCF_CMD_AC };
    esp_err_t ret = (*card->host.do_transaction)(card->host.slot, &cmd32);
    if (ret != ESP_OK) return ret;

    // 2. Set End Address (CMD33)
    sdmmc_command_t cmd33 = { .opcode = 33, .arg = end_sector, .flags = SCF_RSP_R1 | SCF_CMD_AC };
    ret = (*card->host.do_transaction)(card->host.slot, &cmd33);
    if (ret != ESP_OK) return ret;

    // 3. EXECUTE ERASE (CMD38) - NO WAIT_BUSY FLAG
    // This sends the command and returns immediately so the driver doesn't timeout.
    sdmmc_command_t cmd38 = {
        .opcode = 38,
        .arg = 0,
        .flags = SCF_RSP_R1 | SCF_CMD_AC
    };
    ret = (*card->host.do_transaction)(card->host.slot, &cmd38);
    if (ret != ESP_OK) return ret;

    // 4. THE "ARE WE THERE YET" LOOP (CMD13)
    printf("CMD38 accepted. Card is nuking NAND. Polling until ready...\n");

    int retry_count = 3000; // 30 seconds (10ms * 3000)
    while (retry_count > 0) {
        sdmmc_command_t cmd13 = {
            .opcode = 13, // SEND_STATUS
            .arg = card->rca << 16,
            .flags = SCF_RSP_R1 | SCF_CMD_AC
        };

        // If the card is busy erasing, this will return ESP_ERR_TIMEOUT
        // because the driver sees MISO is LOW.
        ret = (*card->host.do_transaction)(card->host.slot, &cmd13);

        if (ret == ESP_OK) {
            printf("Success! Card released the bus. Erase finished.\n");
            return ESP_OK;
        }

        // If it's a timeout, that's EXPECTED. It means it's still busy.
        // If it's any other error, something actually broke.
        if (ret != ESP_ERR_TIMEOUT) {
            printf("Unexpected error during poll: 0x%x\n", ret);
            return ret;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
        retry_count--;
    }

    printf("Erase took longer than 30 seconds. Giving up.\n");
    return ESP_ERR_TIMEOUT;
}



esp_err_t 
_sector_initialization(
    sdmmc_card_t *card, 
    size_t *starting_sector
) {
    uint8_t* buffer = heap_caps_malloc(512, MALLOC_CAP_DMA); // DMA-capable memory
    *starting_sector = SD_INIT_SECTOR;
                                                             
    /* This is optional */
    //ret = _erase_sectors(card, SD_INIT_SECTOR, SD_SECTOR_COUNT);
    //if (ret != ESP_OK) {
    //    ESP_LOGE(SDIF_TAG, "Couldn't remove the sector %s", esp_err_to_name(ret));
    //    return ret;
    //}
    /* */

    /* Align memory to 32KB*/
    *starting_sector = *starting_sector >> 6;
    *starting_sector = (*starting_sector + 1) << 6;
    *starting_sector = find_writeable_sector(card, buffer, *starting_sector, SD_LAST_SECTOR);
    
    if (0 == *starting_sector) {
        heap_caps_free(buffer);
        ESP_LOGE(SDIF_TAG, "Something went wrong when reading the sectors.");
        return ESP_ERR_NOT_FOUND;
    }

    *starting_sector = *starting_sector >> 6;
    *starting_sector = (*starting_sector + 1) << 6;

    heap_caps_free(buffer);
    return ESP_OK;
}

esp_err_t _sd_initialization(spi_host_device_t spi_port, sdmmc_card_t *card) {
    esp_err_t ret;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 20000;   // Force max speed
    host.command_timeout_ms = 10000;
    
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = GPIO_NUM_15;
    slot_cfg.host_id = spi_port;

    sdspi_dev_handle_t handle;

    ret = sdspi_host_init_device(&slot_cfg, &handle); // Link SPI device to host
    if (ret != ESP_OK) {
        ESP_LOGE(SDIF_TAG, "Couldn't bind SPI to host.");
        return ret;
    }
    
    host.slot = (int) handle;
    
    ret = sdmmc_card_init(&host, card); // Perform the SD handshake
    if (ret != ESP_OK) {
        ESP_LOGE(SDIF_TAG, "Couldn't peform SD handshake %s.", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t sys_sd_init(spi_host_device_t spi_port, sdmmc_card_t *card, size_t *starting_sector) {
    esp_log_level_set("sdmmc_init", ESP_LOG_DEBUG);
    esp_log_level_set("sdspi_host", ESP_LOG_DEBUG);
    *starting_sector = SD_INIT_SECTOR;

    if (_sd_initialization(spi_port, card) != ESP_OK) {
        return ESP_FAIL;
    }
   
    return _sector_initialization(card, starting_sector);
}


#elif SDIO
esp_err_t SD_HW_init(void) {
    return ESP_OK;
}


#endif
