/**
 * @file sd_backup.c
 * @brief Received-telemetry backup to the P4's microSD - see sd_backup.h.
 *
 * Mounting follows Espressif's esp_hosted example
 * mcu_hosted_sdio_sdmmc_combined: on IDF >= 6.0 the esp-hosted SDIO transport
 * already owns the SDMMC host controller, so the card driver gets dummy
 * init/deinit hooks and only adds slot 0. Initialising the controller a second
 * time would break the link to the C6.
 */

#include "sd_backup.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#include "bsp/esp-bsp.h"   /* BSP_SD_* pins of this board */

static const char *TAG = "sd_backup";

#define MOUNT_POINT     "/sdcard"
#define LOG_DIR         MOUNT_POINT "/PILOG"
#define SD_LDO_CHAN     4                    /* on-chip LDO feeding the SD slot */
#define RING_CAP        (64 * 1024)          /* ~20 s of telemetry, in PSRAM */
#define DRAIN_MS        200
#define SYNC_MS         2000                 /* data on the card at most this old */

static uint8_t          *s_ring;
static size_t            s_head, s_tail, s_used;
static SemaphoreHandle_t s_lock;
static FILE             *s_file;
static sd_backup_info_t  s_info;

static esp_err_t host_init_dummy(void)   { return ESP_OK; }
static esp_err_t host_deinit_dummy(void) { return ESP_OK; }

/* ── ring (caller holds s_lock) ───────────────────────────────────────── */

static size_t ring_put(const uint8_t *src, size_t len)
{
    size_t room = RING_CAP - s_used;
    if (len > room) len = room;
    size_t first = RING_CAP - s_head;
    if (first > len) first = len;
    memcpy(s_ring + s_head, src, first);
    if (len > first) memcpy(s_ring, src + first, len - first);
    s_head = (s_head + len) % RING_CAP;
    s_used += len;
    return len;
}

static size_t ring_get(uint8_t *dst, size_t max)
{
    size_t len = (s_used < max) ? s_used : max;
    size_t first = RING_CAP - s_tail;
    if (first > len) first = len;
    memcpy(dst, s_ring + s_tail, first);
    if (len > first) memcpy(dst + first, s_ring, len - first);
    s_tail = (s_tail + len) % RING_CAP;
    s_used -= len;
    return len;
}

/* ── writer task ──────────────────────────────────────────────────────── */

static void writer_task(void *arg)
{
    (void)arg;
    static uint8_t chunk[4096];
    TickType_t last_sync = xTaskGetTickCount();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));

        size_t n;
        do {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            n = ring_get(chunk, sizeof(chunk));
            xSemaphoreGive(s_lock);
            if (n && fwrite(chunk, 1, n, s_file) != n) {
                ESP_LOGE(TAG, "write failed - backup stopped (card removed?)");
                fclose(s_file);
                s_file = NULL;
                s_info.active = false;
                vTaskDelete(NULL);
            }
            s_info.bytes += n;
        } while (n == sizeof(chunk));

        /* fsync, not just fflush: a power cut must not lose more than this. */
        if (xTaskGetTickCount() - last_sync >= pdMS_TO_TICKS(SYNC_MS)) {
            fflush(s_file);
            fsync(fileno(s_file));
            last_sync = xTaskGetTickCount();
        }
    }
}

/* ── public ───────────────────────────────────────────────────────────── */

bool sd_backup_start(bool controller_ready)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;             /* slot 1 belongs to the C6 */
    if (controller_ready) {
        host.init   = &host_init_dummy;
        host.deinit = &host_deinit_dummy;
    }

    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = SD_LDO_CHAN };
    sd_pwr_ctrl_handle_t pwr = NULL;
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr) != ESP_OK) {
        ESP_LOGW(TAG, "SD power LDO unavailable - no backup");
        return false;
    }
    host.pwr_ctrl_handle = pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = BSP_SD_CLK;
    slot.cmd = BSP_SD_CMD;
    slot.d0  = BSP_SD_D0;
    slot.d1  = BSP_SD_D1;
    slot.d2  = BSP_SD_D2;
    slot.d3  = BSP_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,     /* never wipe a card on its own */
        .max_files              = 2,
        .allocation_unit_size   = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SD card (%s) - running without backup", esp_err_to_name(err));
        sd_pwr_ctrl_del_on_chip_ldo(pwr);
        return false;
    }

    mkdir(LOG_DIR, 0777);                     /* EEXIST is fine */

    /* Next free RX_nnnn.BIN - one file per boot, never overwrite. */
    char path[48];
    int idx;
    for (idx = 1; idx <= 9999; idx++) {
        snprintf(path, sizeof(path), LOG_DIR "/RX_%04d.BIN", idx);
        struct stat st;
        if (stat(path, &st) != 0) break;
    }
    if (idx > 9999) {
        ESP_LOGE(TAG, "PILOG folder full (9999 files) - no backup");
        return false;
    }

    s_ring = heap_caps_malloc(RING_CAP, MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    s_file = fopen(path, "wb");
    if (!s_ring || !s_lock || !s_file) {
        ESP_LOGE(TAG, "backup setup failed (ring/lock/file)");
        return false;
    }
    snprintf(s_info.name, sizeof(s_info.name), "RX_%04d.BIN", idx);

    if (xTaskCreate(writer_task, "sd_backup", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "writer task create failed - no backup");
        fclose(s_file);
        s_file = NULL;
        return false;
    }
    s_info.active = true;
    ESP_LOGI(TAG, "recording received telemetry to %s (%s, %llu MB card)",
             path, card->cid.name,
             (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    return true;
}

void sd_backup_write(const uint8_t *data, size_t len)
{
    if (!s_info.active || data == NULL || len == 0) return;
    /* Zero wait: the caller is the esp-hosted receive path. */
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        s_info.dropped += len;
        return;
    }
    size_t put = ring_put(data, len);
    xSemaphoreGive(s_lock);
    if (put < len) s_info.dropped += (uint32_t)(len - put);
}

void sd_backup_info(sd_backup_info_t *out)
{
    *out = s_info;
}
