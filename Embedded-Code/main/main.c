/**
 * @file main.c
 * @brief Star-PI Payload — modular sensor orchestrator.
 *
 * Enable / disable peripherals via  `idf.py menuconfig`
 *   → "Star-PI Payload Configuration"
 *
 * Architecture:
 *   Core 1  →  task_sensor_read()   reads all enabled sensors into a ring buffer
 *   Core 0  →  task_sd_write()      drains the ring buffer to the SD card CSV
 *
 * To add a new sensor:
 *   1.  Create  my_sensor.h / my_sensor.c  exporting a `sensor_driver_t`
 *   2.  Add a Kconfig bool  CONFIG_ENABLE_MY_SENSOR
 *   3.  Register it below inside a  #if CONFIG_ENABLE_MY_SENSOR  block
 *   4.  Add the .c to CMakeLists.txt SRCS
 */

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/unistd.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "sdkconfig.h"
#include "sensor_config.h"

/* ── Conditionally include sensor drivers ─────────────────── */
#if CONFIG_ENABLE_I2C_BUS
#include "i2c_bus.h"
#endif

#if CONFIG_ENABLE_MPU6050
#include "mpu6050.h"
#endif

#if CONFIG_ENABLE_BME280
#include "bme280.h"
#endif

#if CONFIG_ENABLE_INA219
#include "ina219.h"
#endif

#if CONFIG_ENABLE_GPS
#include "gps.h"
#endif

/* Frame logger types are always needed for building frames */
#include "frame_logger.h"

static const char *TAG = "main";

/* ═══════════════════════════════════════════════════════════
 *  Sensor registry — filled at boot from enabled drivers
 * ═══════════════════════════════════════════════════════════ */

static const sensor_driver_t *s_sensors[MAX_SENSORS];
static int s_num_sensors = 0;

static void register_sensor(const sensor_driver_t *drv)
{
    if (s_num_sensors >= MAX_SENSORS) {
        ESP_LOGE(TAG, "Too many sensors (max %d)", MAX_SENSORS);
        return;
    }
    s_sensors[s_num_sensors++] = drv;
    ESP_LOGI(TAG, "Registered sensor [%d]: %s  (%d bytes/sample)",
             s_num_sensors - 1, drv->name, drv->data_len);
}

/* ═══════════════════════════════════════════════════════════
 *  Lock-free ring buffer  (single producer / single consumer)
 * ═══════════════════════════════════════════════════════════ */

#define BUFFER_SIZE     4096
#define SAMPLE_SIZE     256     /* generous upper bound per combined sample */

typedef struct {
    uint8_t           data[BUFFER_SIZE];
    atomic_size_t     write_index;
    atomic_size_t     read_index;
    SemaphoreHandle_t data_available;
} RingBuffer_t;

static RingBuffer_t ring_buffer;

static void ring_buffer_init(void)
{
    memset(ring_buffer.data, 0, BUFFER_SIZE);
    atomic_store(&ring_buffer.write_index, 0);
    atomic_store(&ring_buffer.read_index, 0);
    ring_buffer.data_available = xSemaphoreCreateCounting(BUFFER_SIZE / SAMPLE_SIZE, 0);
    ESP_LOGI(TAG, "Ring buffer ready (%d bytes)", BUFFER_SIZE);
}

static inline size_t ring_buffer_free(void)
{
    return BUFFER_SIZE - (atomic_load(&ring_buffer.write_index) -
                          atomic_load(&ring_buffer.read_index));
}

static inline size_t ring_buffer_available(void)
{
    return atomic_load(&ring_buffer.write_index) -
           atomic_load(&ring_buffer.read_index);
}

static size_t ring_buffer_write(const uint8_t *src, size_t len)
{
    if (ring_buffer_free() < len) {
        ESP_LOGW(TAG, "Ring buffer full — dropping %d bytes", (int)len);
        return 0;
    }
    size_t w = atomic_load(&ring_buffer.write_index);
    for (size_t i = 0; i < len; i++)
        ring_buffer.data[(w + i) % BUFFER_SIZE] = src[i];
    atomic_store(&ring_buffer.write_index, w + len);
    xSemaphoreGive(ring_buffer.data_available);
    return len;
}

static size_t ring_buffer_read(uint8_t *dst, size_t max_len)
{
    size_t avail = ring_buffer_available();
    if (avail == 0) return 0;
    size_t n = (max_len < avail) ? max_len : avail;
    size_t r = atomic_load(&ring_buffer.read_index);
    for (size_t i = 0; i < n; i++)
        dst[i] = ring_buffer.data[(r + i) % BUFFER_SIZE];
    atomic_store(&ring_buffer.read_index, r + n);
    return n;
}

/* ═══════════════════════════════════════════════════════════
 *  Core 1 — sensor read task (builds binary frames)
 * ═══════════════════════════════════════════════════════════ */

static void task_sensor_read(void *arg)
{
    ESP_LOGI(TAG, "Sensor task running on core %d", xPortGetCoreID());

    uint16_t frame_id = 0;
    frame_builder_t fb;
    uint8_t tmp[SENSOR_MAX_DATA_LEN];

    while (1) {
        /* Start new frame */
        frame_begin(&fb, frame_id);

        /* Timestamp (ms) */
        uint32_t ts = xTaskGetTickCount() * portTICK_PERIOD_MS;
        frame_add_timestamp(&fb, ts);

        /* MPU6050: Accelerometer + Gyroscope + Temperature */
#if CONFIG_ENABLE_MPU6050
        {
            uint8_t mpu_data[14];
            esp_err_t ret = mpu6050_read(mpu_data);
            if (ret == ESP_OK) {
                /* Accel: bytes 0-5 (X, Y, Z as int16 big-endian) */
                frame_add_accel(&fb, mpu_data);
                
                /* Gyro: bytes 8-13 (X, Y, Z as int16 big-endian) */
                frame_add_gyro(&fb, mpu_data + 8);
                
                /* Temp from MPU: bytes 6-7, expand to 4 bytes */
                uint8_t temp_data[4] = {mpu_data[6], mpu_data[7], 0, 0};
                frame_add_temperature(&fb, temp_data);
            } else {
                ESP_LOGW(TAG, "MPU6050 read failed: %s", esp_err_to_name(ret));
            }
        }
#endif

        /* BME280: Pressure + Temperature + Humidity */
#if CONFIG_ENABLE_BME280
        {
            uint8_t bme_data[8];
            esp_err_t ret = bme280_read(bme_data);
            if (ret == ESP_OK) {
                /* Pressure: bytes 0-2 (20-bit), expand to 4 bytes */
                uint8_t press_data[4] = {bme_data[0], bme_data[1], bme_data[2], 0};
                frame_add_pressure(&fb, press_data);
                
                /* Humidity: bytes 6-7 */
                frame_add_humidity(&fb, bme_data + 6);
                
                /* If MPU6050 not present, use BME280 temperature */
#if !CONFIG_ENABLE_MPU6050
                uint8_t temp_data[4] = {bme_data[3], bme_data[4], bme_data[5], 0};
                frame_add_temperature(&fb, temp_data);
#endif
            } else {
                ESP_LOGW(TAG, "BME280 read failed: %s", esp_err_to_name(ret));
            }
        }
#endif

        /* INA219: Power monitor data */
#if CONFIG_ENABLE_INA219
        {
            uint8_t ina_data[8];
            esp_err_t ret = ina219_read(ina_data);
            if (ret == ESP_OK) {
                frame_add_power(&fb, ina_data);
            } else {
                ESP_LOGW(TAG, "INA219 read failed: %s", esp_err_to_name(ret));
            }
        }
#endif

        /* GPS: Raw NMEA data */
#if CONFIG_ENABLE_GPS
        {
            uint8_t gps_data[GPS_MAX_SENTENCE_LEN];
            esp_err_t ret = gps_read(gps_data);
            if (ret == ESP_OK) {
                size_t gps_len = strnlen((char *)gps_data, GPS_MAX_SENTENCE_LEN);
                frame_add_gps(&fb, gps_data, gps_len);
            }
            /* GPS_ERR_NOT_FOUND is normal when no new data */
        }
#endif

        /* Finalize frame (adds CRC) */
        size_t frame_len = frame_finish(&fb);

        /* Write frame to ring buffer */
        if (ring_buffer_write(frame_get_data(&fb), frame_len) > 0) {
            frame_id++;
            if (frame_id % 500 == 0) {
                ESP_LOGI(TAG, "Frames: %u", frame_id);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_SAMPLE_INTERVAL_MS));
    }
}

/* ═══════════════════════════════════════════════════════════
 *  Core 0 — SD card write task (writes binary frames)
 * ═══════════════════════════════════════════════════════════ */

#if CONFIG_ENABLE_SD_CARD
static void task_sd_write(void *arg)
{
    ESP_LOGI(TAG, "SD write task running on core %d", xPortGetCoreID());

    uint8_t buf[FRAME_MAX_SIZE];
    uint32_t frames_written = 0;

    while (1) {
        if (xSemaphoreTake(ring_buffer.data_available, pdMS_TO_TICKS(1000)) != pdTRUE)
            continue;

        /* Read frame from ring buffer */
        size_t n = ring_buffer_read(buf, FRAME_MAX_SIZE);
        if (n == 0) continue;

        /* Write raw binary frame to file */
        FILE *f = frame_logger_get_file();
        if (f && fwrite(buf, 1, n, f) == n) {
            frames_written++;
        }

        if (frames_written % 100 == 0) {
            frame_logger_flush();
            ESP_LOGI(TAG, "SD: %lu frames written", (unsigned long)frames_written);
        }
    }
}
#endif /* CONFIG_ENABLE_SD_CARD */

/* ═══════════════════════════════════════════════════════════
 *  app_main
 * ═══════════════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════════╗");
    ESP_LOGI(TAG, "║     Star-PI Payload  v2.0        ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════╝");

    /* ── 1. I2C bus (shared by all I2C sensors) ──────────── */
#if CONFIG_ENABLE_I2C_BUS
    ESP_ERROR_CHECK(i2c_bus_init());
    i2c_master_bus_handle_t bus = i2c_bus_get_handle();
#endif

    /* ── 2. Register enabled sensors ─────────────────────── */
#if CONFIG_ENABLE_MPU6050
    if (mpu6050_init(bus) == ESP_OK)
        register_sensor(&mpu6050_driver);
    else
        ESP_LOGW(TAG, "MPU6050 init failed — skipping");
#endif

#if CONFIG_ENABLE_BME280
    if (bme280_init(bus) == ESP_OK)
        register_sensor(&bme280_driver);
    else
        ESP_LOGW(TAG, "BME280 init failed — skipping");
#endif

#if CONFIG_ENABLE_INA219
    if (ina219_init(bus) == ESP_OK)
        register_sensor(&ina219_driver);
    else
        ESP_LOGW(TAG, "INA219 init failed — skipping");
#endif

#if CONFIG_ENABLE_GPS
    if (gps_init(NULL) == ESP_OK) {
        register_sensor(&gps_driver);
        gps_start_task();
    } else {
        ESP_LOGW(TAG, "GPS init failed — skipping");
    }
#endif

    ESP_LOGI(TAG, "Active sensors: %d", s_num_sensors);

    if (s_num_sensors == 0) {
        ESP_LOGW(TAG, "No sensors enabled! Enable at least one via menuconfig.");
    }

    /* ── 3. SD card (binary frame logger) ────────────────── */
#if CONFIG_ENABLE_SD_CARD
    esp_err_t sd_ret = frame_logger_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed — data will NOT be logged!");
    }
#endif

    /* ── 4. Ring buffer ──────────────────────────────────── */
    ring_buffer_init();

    /* ── 5. Start tasks on separate cores ────────────────── */
    static TaskHandle_t sensor_task_h = NULL;
    xTaskCreatePinnedToCore(task_sensor_read, "sensor_rd", 4096, NULL, 6, &sensor_task_h, 1);

#if CONFIG_ENABLE_SD_CARD
    if (sd_ret == ESP_OK) {
        static TaskHandle_t sd_task_h = NULL;
        xTaskCreatePinnedToCore(task_sd_write, "sd_wr", 8192, NULL, 5, &sd_task_h, 0);
    }
#endif

    ESP_LOGI(TAG, "System running — sampling every %d ms", CONFIG_SAMPLE_INTERVAL_MS);

    /* ── 6. Heartbeat / watchdog loop ────────────────────── */
    while (1) {
        ESP_LOGI(TAG, "Heartbeat | buf=%d bytes | sensors=%d",(int)ring_buffer_available(), s_num_sensors);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    /* unreachable, but good practice */
#if CONFIG_ENABLE_SD_CARD
    frame_logger_deinit();
#endif
}
