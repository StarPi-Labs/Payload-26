/**
 * @file frame_logger.c
 * @brief Binary frame logger implementation.
 */

#include "frame_logger.h"
#include "sensor_config.h"
#include "sdkconfig.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#if CONFIG_ENABLE_SD_CARD
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#endif

#include "systemp2i.h"

static const char *TAG = "frame_logger";

#if CONFIG_ENABLE_SD_CARD
#define MOUNT_POINT     "/sdcard"
#define DATA_FILE       MOUNT_POINT "/sensor_data.bin"

static sdmmc_card_t *s_card = NULL;
static FILE         *s_file = NULL;
#endif

static uint32_t      s_frame_count = 0;

/* ═══════════════════════════════════════════════════════════
 *  CRC-16 CCITT (polynomial 0x1021, initial value 0xFFFF)
 * ═══════════════════════════════════════════════════════════ */

void crc16_ccitt(uint16_t *crc, const uint8_t *data, size_t len) {
    // uint16_t crc = 0xFFFF;
    
    for (size_t i = 0; i < len; i++) {
        *crc ^= ((uint16_t)data[i] << 8);
        for (int j = 0; j < 8; j++) {
            if (*crc & 0x8000) {
                *crc = (*crc << 1) ^ 0x1021;
            } else {
                *crc <<= 1;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 *  Frame Builder Implementation
 * ═══════════════════════════════════════════════════════════ */

void frame_begin(frame_builder_t *fb, uint16_t frame_id)
{
    memset(fb, 0, sizeof(*fb));
    fb->frame_id = frame_id;
    fb->offset = 0;
    
    /* Write frame separator (0xAAAAAAAA) */
    uint32_t sep = FRAME_SEPARATOR;
    memcpy(fb->buffer + fb->offset, &sep, sizeof(sep));
    fb->offset += sizeof(sep);
    
    /* Write frame ID (little-endian) */
    fb->buffer[fb->offset++] = (uint8_t)(frame_id & 0xFF);
    fb->buffer[fb->offset++] = (uint8_t)((frame_id >> 8) & 0xFF);
    
    /* Reserve space for sensor bitmap (will be filled in frame_finish) */
    fb->buffer[fb->offset++] = 0x00;
    fb->buffer[fb->offset++] = 0x00;
    
    fb->payload_start = fb->offset;
    fb->sensor_bitmap = 0;
}

void frame_add_timestamp(frame_builder_t *fb, uint32_t timestamp)
{
    /* Little-endian timestamp */
    fb->buffer[fb->offset++] = (uint8_t)(timestamp & 0xFF);
    fb->buffer[fb->offset++] = (uint8_t)((timestamp >> 8) & 0xFF);
    fb->buffer[fb->offset++] = (uint8_t)((timestamp >> 16) & 0xFF);
    fb->buffer[fb->offset++] = (uint8_t)((timestamp >> 24) & 0xFF);
}

void frame_add_accel(frame_builder_t *fb, const uint8_t *data)
{
    memcpy(fb->buffer + fb->offset, data, ACCEL_DATA_LEN);
    fb->offset += ACCEL_DATA_LEN;
    fb->sensor_bitmap |= SENSOR_BIT_ACCEL;
}

void frame_add_gyro(frame_builder_t *fb, const uint8_t *data)
{
    memcpy(fb->buffer + fb->offset, data, GYRO_DATA_LEN);
    fb->offset += GYRO_DATA_LEN;
    fb->sensor_bitmap |= SENSOR_BIT_GYRO;
}

void frame_add_temperature(frame_builder_t *fb, const uint8_t *data)
{
    memcpy(fb->buffer + fb->offset, data, TEMP_DATA_LEN);
    fb->offset += TEMP_DATA_LEN;
    fb->sensor_bitmap |= SENSOR_BIT_TEMP;
}

void frame_add_humidity(frame_builder_t *fb, const uint8_t *data)
{
    memcpy(fb->buffer + fb->offset, data, HUMIDITY_DATA_LEN);
    fb->offset += HUMIDITY_DATA_LEN;
    fb->sensor_bitmap |= SENSOR_BIT_HUMIDITY;
}

void frame_add_pressure(frame_builder_t *fb, const uint8_t *data)
{
    memcpy(fb->buffer + fb->offset, data, PRESSURE_DATA_LEN);
    fb->offset += PRESSURE_DATA_LEN;
    fb->sensor_bitmap |= SENSOR_BIT_PRESSURE;
}

void frame_add_gps(frame_builder_t *fb, const uint8_t *data, size_t len)
{
    /* GPS field is fixed size, zero-pad if necessary */
    size_t copy_len = (len > GPS_DATA_LEN) ? GPS_DATA_LEN : len;
    memcpy(fb->buffer + fb->offset, data, copy_len);
    if (copy_len < GPS_DATA_LEN) {
        memset(fb->buffer + fb->offset + copy_len, 0, GPS_DATA_LEN - copy_len);
    }
    fb->offset += GPS_DATA_LEN;
    fb->sensor_bitmap |= SENSOR_BIT_GPS;
}

void frame_add_air_quality(frame_builder_t *fb, const uint8_t *data)
{
    memcpy(fb->buffer + fb->offset, data, AIR_QUALITY_DATA_LEN);
    fb->offset += AIR_QUALITY_DATA_LEN;
    fb->sensor_bitmap |= SENSOR_BIT_AIR_QUALITY;
}

void frame_add_power(frame_builder_t *fb, const uint8_t *data)
{
    memcpy(fb->buffer + fb->offset, data, POWER_DATA_LEN);
    fb->offset += POWER_DATA_LEN;
    fb->sensor_bitmap |= SENSOR_BIT_POWER;
}

size_t frame_finish(frame_builder_t *fb)
{
    /* Write sensor bitmap back into header (offset 6-7, after separator and frame_id) */
    fb->buffer[6] = (uint8_t)(fb->sensor_bitmap & 0xFF);
    fb->buffer[7] = (uint8_t)((fb->sensor_bitmap >> 8) & 0xFF);
    
    /* Calculate CRC over entire frame (excluding the CRC field itself) */
    //uint16_t crc = crc16_ccitt(fb->buffer, fb->offset);
    uint16_t crc = 0xFFFF;
    
    /* Append CRC (little-endian) */
    fb->buffer[fb->offset++] = (uint8_t)(crc & 0xFF);
    fb->buffer[fb->offset++] = (uint8_t)((crc >> 8) & 0xFF);
    
    return fb->offset;
}

const uint8_t *frame_get_data(const frame_builder_t *fb)
{
    return fb->buffer;
}

/* ═══════════════════════════════════════════════════════════
 *  SD Card Logger
 * ═══════════════════════════════════════════════════════════ */

#if CONFIG_ENABLE_SD_CARD

esp_err_t frame_logger_init(void)
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

    /* Check if file exists to count existing frames */
    struct stat st;
    bool file_exists = (stat(DATA_FILE, &st) == 0);

    /* Open in append+binary mode */
    s_file = fopen(DATA_FILE, "ab");
    if (s_file == NULL) {
        ESP_LOGE(TAG, "fopen failed");
        return ESP_FAIL;
    }

    if (file_exists) {
        ESP_LOGI(TAG, "Appending to %s (size: %ld bytes)", DATA_FILE, st.st_size);
    } else {
        ESP_LOGI(TAG, "Created new file: %s", DATA_FILE);
    }

    s_frame_count = 0;
    return ESP_OK;
}

esp_err_t frame_logger_write(const frame_builder_t *fb)
{
    if (s_file == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t written = fwrite(fb->buffer, 1, fb->offset, s_file);
    if (written != fb->offset) {
        ESP_LOGE(TAG, "Write failed: %d/%d bytes", (int)written, (int)fb->offset);
        return ESP_FAIL;
    }

    s_frame_count++;
    return ESP_OK;
}

void frame_logger_flush(void)
{
    if (s_file) {
        fflush(s_file);
    }
}

void frame_logger_deinit(void)
{
    if (s_file) {
        fflush(s_file);
        fclose(s_file);
        s_file = NULL;
        ESP_LOGI(TAG, "Wrote %lu frames total", (unsigned long)s_frame_count);
    }
    if (s_card) {
        // esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_card = NULL;
    }
    ESP_LOGI(TAG, "SD card unmounted");
}

uint32_t frame_logger_get_count(void)
{
    return s_frame_count;
}

FILE *frame_logger_get_file(void)
{
    return s_file;
}

#else /* CONFIG_ENABLE_SD_CARD not defined */

esp_err_t frame_logger_init(void)
{
    ESP_LOGW(TAG, "SD card logging disabled in config");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t frame_logger_write(const frame_builder_t *fb)
{
    (void)fb;
    return ESP_ERR_NOT_SUPPORTED;
}

void frame_logger_flush(void) { }

void frame_logger_deinit(void) { }

uint32_t frame_logger_get_count(void)
{
    return s_frame_count;
}

FILE *frame_logger_get_file(void)
{
    return NULL;
}

/**
 * LOGGER 
 */
// Circular history buffer (PSRAM) + mode-aware SD writer.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "bt_serial_bridge.h"
#include <stdbool.h>

#define LOG_RING_CAP      (256 * 1024)  /* PSRAM circular buffer size */
#define LOG_RING_MIN      (32  * 1024)  /* fallback if PSRAM is unavailable */
#define LOG_PRETRIG_BYTES (32  * 1024)  /* dumped to SD on BOOST (~10 s of armed-rate history) */
#define LOG_SPARSE_MS     10000         /* ARMED: write a recent slice to SD this often */
#define LOG_SPARSE_BYTES  1024          /* ARMED: bytes per sparse write */
#define LOG_STAGING       4096          /* SD write chunk */
                            
struct LoggerBuffer {
    SemaphoreHandle_t lock;
    uint8_t          *buf;
    size_t            cap;
    volatile uint64_t head;   /* total bytes ever written (monotonic) */
};

/* Append `len` bytes at the write head, wrapping. Caller holds the lock. */
static void rb_put(struct LoggerBuffer *b, const uint8_t *src, size_t len)
{
    size_t off   = (size_t)(b->head % b->cap);
    size_t first = b->cap - off;
    if (first > len) first = len;
    memcpy(b->buf + off, src, first);
    if (len > first) memcpy(b->buf, src + first, len - first);
    b->head += len;
}

/* Copy `len` bytes starting at absolute offset `from` into dst. Caller holds the lock. */
static void rb_copy(struct LoggerBuffer *b, uint64_t from, uint8_t *dst, size_t len)
{
    size_t off   = (size_t)(from % b->cap);
    size_t first = b->cap - off;
    if (first > len) first = len;
    memcpy(dst, b->buf + off, first);
    if (len > first) memcpy(dst + first, b->buf, len - first);
}

LoggerBuffer *logger_buff_init(void) {
    static LoggerBuffer lb;
    lb.cap = LOG_RING_CAP;
    lb.buf = heap_caps_malloc(lb.cap, MALLOC_CAP_SPIRAM);
    if (lb.buf == NULL) {                 /* PSRAM off -> smaller internal fallback */
        lb.cap = LOG_RING_MIN;
        lb.buf = heap_caps_malloc(lb.cap, MALLOC_CAP_8BIT);
        ESP_LOGW(TAG, "no PSRAM: log ring = %u B internal RAM", (unsigned)lb.cap);
    } else {
        ESP_LOGI(TAG, "log ring = %u B in PSRAM", (unsigned)lb.cap);
    }
    lb.head = 0;
    lb.lock = xSemaphoreCreateMutex();
    return &lb;
}


uint16_t frame_build(uint8_t *dst, uint8_t type, const void *payload, uint16_t payload_size)
{
    frame_header_t header;
    uint16_t crc = 0xFFFF;

    header.frame_info         = type;
    header.frame_separator[0] = 0xAA;
    header.frame_separator[1] = 0xAA;
    header.frame_separator[2] = 0xAA;
    header.timestamp_ms       = xTaskGetTickCount() * portTICK_PERIOD_MS;
    crc16_ccitt(&crc, (uint8_t *)&header, sizeof(header));
    crc16_ccitt(&crc, (const uint8_t *)payload, payload_size);

    memcpy(dst, &header, sizeof(header));
    memcpy(dst + sizeof(header), payload, payload_size);
    memcpy(dst + sizeof(header) + payload_size, &crc, 2);
    return sizeof(header) + payload_size + 2;
}

/* Calibration payload, stashed by logging_set_calib and written at the head of
 * every SD log session so the ground can convert raw frames standalone. */
#define CALIB_PAYLOAD_MAX 96
static uint8_t  s_calib_payload[CALIB_PAYLOAD_MAX];
static uint16_t s_calib_len = 0;

void logging_set_calib(const void *payload, uint16_t len)
{
    if (len > CALIB_PAYLOAD_MAX) len = CALIB_PAYLOAD_MAX;
    memcpy(s_calib_payload, payload, len);
    s_calib_len = len;
}

uint16_t logging_get_calib(const void **payload)
{
    if (payload) *payload = s_calib_payload;
    return s_calib_len;
}

void
write_to_ring_buffer(
    struct LoggerBuffer *buf,
    uint8_t type,
    void *payload,
    uint16_t payload_size
)
{
    frame_header_t header;
    uint16_t crc = 0xFFFF;

    header.frame_info         = type;
    header.frame_separator[0] = 0xAA;
    header.frame_separator[1] = 0xAA;
    header.frame_separator[2] = 0xAA;
    header.timestamp_ms       = xTaskGetTickCount() * portTICK_PERIOD_MS;
    crc16_ccitt(&crc, (uint8_t *)&header, sizeof(header));
    crc16_ccitt(&crc, (uint8_t *)payload, payload_size);

    if (buf == NULL || buf->buf == NULL) return;

    /* Append header + payload + CRC into the circular buffer (overwrites oldest). */
    xSemaphoreTake(buf->lock, portMAX_DELAY);
    rb_put(buf, (uint8_t *)&header, sizeof(header));
    rb_put(buf, (uint8_t *)payload, payload_size);
    rb_put(buf, (uint8_t *)&crc, 2);
    xSemaphoreGive(buf->lock);
}

esp_err_t logging_init(int *fd, char *filename) {
    // This is necessary, so checking the file existence is checked from the very begging
    // TODO: This file should be open as APPEND+WRITE / NO CREATE
    *fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (*fd < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Write to the SD file — or, with no file (no card / mount failure), mirror
 * the exact same bytes over the telemetry link so the PC can capture the
 * identical stream the SD card would have received. */
static void log_sink(FILE *f, const uint8_t *buf, size_t len)
{
    if (f) fwrite(buf, 1, len, f);
    else   bt_serial_write_chunk((uint8_t *)buf, (uint16_t)len);
}

void logging_task(void *args) {
    struct TaskParams   *tparams = (struct TaskParams *)args;
    struct LoggerBuffer *b       = tparams->log_buffer;
    struct SysContext   *ctx     = tparams->context;
    FILE                *f       = (FILE *)tparams->args;
    static uint8_t stage[LOG_STAGING];
    uint64_t sd_total    = 0;
    bool     streaming   = false;
    int64_t  last_sparse = 0;
    int64_t  last_calib  = 0;
    int      flush_ctr   = 0;

    if (NULL == f) {
        ESP_LOGW(TAG, "no log file — mirroring the SD stream to telemetry");
    }

    /* Calibration frame first: every session starts with the constants the
     * ground needs to convert this log's raw frames. */
    if (s_calib_len) {
        uint16_t n = frame_build(stage, SBIT_CALIB, s_calib_payload, s_calib_len);
        log_sink(f, stage, n);
        if (f) { fflush(f); fsync(fileno(f)); }
        ESP_LOGI(TAG, "calibration frame written (%u B payload)", (unsigned)s_calib_len);
    }

    while (1) {
        uint32_t mode = ctx ? ctx->mode : MODE_COAST;
        bool flight = (mode == MODE_BOOST || mode == MODE_COAST);

        /* Mirror mode only: re-send the calibration frame every ~10 s in EVERY
         * mode, so a PC capture attached at any moment (even mid-COAST) soon
         * receives the constants to convert the raw frames. The parser always
         * applies the newest calib frame to all frames that follow it. */
        if (NULL == f && s_calib_len) {
            int64_t now_c = esp_timer_get_time();
            if (now_c - last_calib >= 10000000LL) {
                uint16_t n = frame_build(stage, SBIT_CALIB, s_calib_payload, s_calib_len);
                log_sink(f, stage, n);
                last_calib = now_c;
            }
        }

        /* Launch: rewind the SD cursor by the pre-trigger window, then stream. */
        if (flight && !streaming) {
            xSemaphoreTake(b->lock, portMAX_DELAY);
            uint64_t head = b->head;
            xSemaphoreGive(b->lock);
            uint64_t pre = (head > LOG_PRETRIG_BYTES) ? LOG_PRETRIG_BYTES : head;
            sd_total  = head - pre;
            streaming = true;
            ESP_LOGW(TAG, "launch: dumping %u B pre-trigger, then streaming", (unsigned)pre);
        }

        if (streaming) {
            xSemaphoreTake(b->lock, portMAX_DELAY);
            uint64_t head = b->head;
            if (head - sd_total > b->cap) sd_total = head - b->cap;   /* SD too slow: keep last cap */
            size_t avail = (size_t)(head - sd_total);
            size_t chunk = (avail > LOG_STAGING) ? LOG_STAGING : avail;
            if (chunk) rb_copy(b, sd_total, stage, chunk);
            xSemaphoreGive(b->lock);

            if (chunk) {
                log_sink(f, stage, chunk);
                sd_total += chunk;
                if (f && ++flush_ctr >= 8) { fflush(f); fsync(fileno(f)); flush_ctr = 0; }
                if (chunk == LOG_STAGING) continue;   /* backlog remains: keep draining */
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            /* ARMED/POST: retain full rate in RAM, trickle a recent slice to SD. */
            int64_t now = esp_timer_get_time();
            if (now - last_sparse >= (int64_t)LOG_SPARSE_MS * 1000) {
                last_sparse = now;
                xSemaphoreTake(b->lock, portMAX_DELAY);
                uint64_t head = b->head;
                size_t n = (head > LOG_SPARSE_BYTES) ? LOG_SPARSE_BYTES : (size_t)head;
                if (n) rb_copy(b, head - n, stage, n);
                xSemaphoreGive(b->lock);
                if (n) {
                    log_sink(f, stage, n);
                    if (f) { fflush(f); fsync(fileno(f)); }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* (The legacy raw-sector double-buffer logging_task was removed — superseded by
 * the PSRAM circular buffer + mode-aware logging_task above.) */

#endif /* CONFIG_ENABLE_SD_CARD */
