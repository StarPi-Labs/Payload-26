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

#if CONFIG_ENABLE_SD_CARD
#include "sd_logger.h"
#endif

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
 *  Core 1 — sensor read task
 * ═══════════════════════════════════════════════════════════ */

static void task_sensor_read(void *arg)
{
    ESP_LOGI(TAG, "Sensor task running on core %d", xPortGetCoreID());

    uint32_t sample_num = 0;
    uint8_t  combined[SAMPLE_SIZE];
    uint8_t  tmp[SENSOR_MAX_DATA_LEN];

    while (1) {
        size_t offset = 0;

        /* Timestamp (ms) */
        uint32_t ts = xTaskGetTickCount() * portTICK_PERIOD_MS;
        memcpy(combined + offset, &ts, sizeof(ts));
        offset += sizeof(ts);

        /* Sample counter */
        memcpy(combined + offset, &sample_num, sizeof(sample_num));
        offset += sizeof(sample_num);

        /* Read every registered sensor */
        for (int i = 0; i < s_num_sensors; i++) {
            const sensor_driver_t *drv = s_sensors[i];
            esp_err_t ret = drv->read(tmp);
            if (ret == ESP_OK) {
                memcpy(combined + offset, tmp, drv->data_len);
            } else {
                memset(combined + offset, 0xFF, drv->data_len);
                if (ret != ESP_ERR_NOT_FOUND) {  /* GPS returns NOT_FOUND when no data yet */
                    ESP_LOGW(TAG, "%s read failed: %s", drv->name, esp_err_to_name(ret));
                }
            }
            offset += drv->data_len;
        }

        if (ring_buffer_write(combined, offset) > 0) {
            sample_num++;
            if (sample_num % 500 == 0) {
                ESP_LOGI(TAG, "Samples: %lu", (unsigned long)sample_num);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_SAMPLE_INTERVAL_MS));
    }
}

/* ═══════════════════════════════════════════════════════════
 *  Core 0 — SD card write task
 * ═══════════════════════════════════════════════════════════ */

#if CONFIG_ENABLE_SD_CARD
static void task_sd_write(void *arg)
{
    ESP_LOGI(TAG, "SD write task running on core %d", xPortGetCoreID());

    FILE *f = sd_logger_get_file();
    uint8_t buf[SAMPLE_SIZE];
    uint32_t lines = 0;

    while (1) {
        if (xSemaphoreTake(ring_buffer.data_available, pdMS_TO_TICKS(1000)) != pdTRUE)
            continue;

        size_t n = ring_buffer_read(buf, SAMPLE_SIZE);
        if (n == 0 || f == NULL) continue;

        /* Parse header */
        uint32_t ts, sn;
        memcpy(&ts, buf, sizeof(ts));
        memcpy(&sn, buf + 4, sizeof(sn));
        fprintf(f, "%lu,%lu", (unsigned long)ts, (unsigned long)sn);

        /* Sensor data bytes */
        size_t off = 8;
        for (int i = 0; i < s_num_sensors; i++) {
            for (int j = 0; j < s_sensors[i]->data_len; j++) {
                fprintf(f, ",%d", buf[off + j]);
            }
            off += s_sensors[i]->data_len;
        }
        fprintf(f, "\n");

        if (++lines % 100 == 0) {
            sd_logger_flush();
            ESP_LOGI(TAG, "SD: %lu lines written", (unsigned long)lines);
        }
    }
}
#endif /* CONFIG_ENABLE_SD_CARD */

/* ═══════════════════════════════════════════════════════════
 *  Build CSV header from registered sensors
 * ═══════════════════════════════════════════════════════════ */

static void build_csv_header(char *buf, size_t buf_len)
{
    size_t pos = 0;
    pos += snprintf(buf + pos, buf_len - pos, "timestamp_ms,sample_num");
    for (int i = 0; i < s_num_sensors; i++) {
        for (int j = 0; j < s_sensors[i]->data_len; j++) {
            pos += snprintf(buf + pos, buf_len - pos, ",%s_b%d", s_sensors[i]->name, j);
        }
    }
}

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

    /* ── 3. SD card ──────────────────────────────────────── */
#if CONFIG_ENABLE_SD_CARD
    char csv_hdr[512];
    build_csv_header(csv_hdr, sizeof(csv_hdr));
    esp_err_t sd_ret = sd_logger_init(csv_hdr);
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
        ESP_LOGI(TAG, "Heartbeat | buf=%d bytes | sensors=%d",
                 (int)ring_buffer_available(), s_num_sensors);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    /* unreachable, but good practice */
#if CONFIG_ENABLE_SD_CARD
    sd_logger_deinit();
#endif
}
