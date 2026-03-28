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

#if CONFIG_ENABLE_SD_CARD
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#endif

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
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
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
// This buffer is used for storage
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#define SECTOR_SIZE 4096    // 8 blocks of SD card (512 bytes each).
struct LoggerBuffer {
    volatile uint8_t bufA[SECTOR_SIZE];
    volatile uint8_t bufB[SECTOR_SIZE];
    volatile uint8_t *active_fill_buffer;
    volatile uint8_t *active_save_buffer;
    volatile uint32_t fill_index;
    portMUX_TYPE guard;
    SemaphoreHandle_t ready;
};

LoggerBuffer *logger_buff_init(void) {
    static LoggerBuffer buf;
    portMUX_INITIALIZE(&buf.guard);
    buf.ready = xSemaphoreCreateBinary();
    buf.active_fill_buffer = buf.bufA;
    buf.active_save_buffer = NULL;
    buf.fill_index = 0;
    return &buf;
}


void write_to_ring_buffer(uint8_t packet, void *data, uint16_t payload_size) {
    /*
    uint16_t total_sz = sizeof(frame_header_t) + payload_size;
    uint8_t *my_write_pointer = NULL;
    frame_header_t header;

    taskENTER_CRITICAL(&buffer_guard);

    if (fill_index + total_sz > SECTOR_SZ) {
        // pad the missing space
        memset(&active_fill_buffer[fill_index], 0, SECTOR_SZ - fill_index);

        active_save_buffer = active_fill_buffer;
        active_fill_buffer = (active_fill_buffer == buffer_A) \
            ? buffer_B : buffer_A;
        fill_index = 0;

        // Wake up the logger
        xSemaphoreGiveFromISR(buffer_ready_singal, NULL);
    } 

    my_write_pointer = &active_fill_buffer[fill_index];
    fill_index += total_sz;
    taskEXIT_CRITICAL(&buffer_guard);

    header.type = type;
    header.timestamp_ms = xTaskGetTickCount * portTICK_PERIOD_MS;
    memcpy(my_write_pointer, &header, sizeof(header));
    memcpy(my_write_pointer + sizeof(header), payload, payload_size);
    */

}

#endif /* CONFIG_ENABLE_SD_CARD */
