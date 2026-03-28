
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
#include "systemp2i.h"
#include "health_monitoring.h"

/* ── Conditionally include sensor drivers ─────────────────── */
#if CONFIG_ENABLE_I2C_BUS
#include "i2c_bus.h"
#endif

#if CONFIG_ENABLE_MPU6050
#include "mpu6050.h"
#endif

#if CONFIG_ENABLE_BME680
#include "bme680.h"
#endif

#if CONFIG_ENABLE_INA219
#include "ina219.h"
#endif

#if CONFIG_ENABLE_GPS
#include "uart.h"
#include "gps.h"
#endif

/* Frame logger types are always needed for building frames */
#include "frame_logger.h"

static const char *TAG = "main";

/* Global Variables */
RTC_DATA_ATTR FlightRecord global_flight_record;

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
        // ESP_LOGW(TAG, "Ring buffer full — dropping %d bytes", (int)len);
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
    System *sys = (System *) arg;
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
        if (sys->health & (1 << MPU6050_HEALTH)) {
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
#if CONFIG_ENABLE_BME680
        if (sys->health & (1 << BME680_HEALTH)) {
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
        if (sys->health & (1 << INA219_HEALTH)) {
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
        if (sys->health & (1 << GPS_HEALTH)) {
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
void hm_start_task(System *sys) {
    static TaskHandle_t hm_handler = NULL;
    static struct TaskParams hm_params;
    hm_params.hm_buffer = sys->hm_buffer;
    hm_params.context = NULL;
    hm_params.log_buffer = NULL;

    xTaskCreatePinnedToCore(
        health_monitoring_task,
        "health_monitoring_task",
        4096,
        (void *) &hm_params,
        6,
        &hm_handler,
        GPS_CORE
    );
    // TODO: sys->tasks.hm = &hm_handler;

}
void gps_start_task(System *sys) {
    static TaskHandle_t gps_handler = NULL; 
    static struct TaskParams gps_params;
    gps_params.hm_buffer  = sys->hm_buffer;
    gps_params.log_buffer = sys->log_buffer;
    gps_params.context = &sys->context;

    xTaskCreatePinnedToCore(
        gps_rx_task, 
        "gps_rx", 
        4096, 
        (void *) &gps_params,
        5, 
        &gps_handler, 
        GPS_CORE
    );

    // TODO: link this taks to system
    // sys->tasks.gps = &gps_handler;
}

void sys_init(System *sys) {
    sys->context.mode = MODE_INIT;
    sys->record = &global_flight_record;
    esp_reset_reason_t reason = esp_reset_reason();

    if (reason == ESP_RST_POWERON) {
        memset(sys->record, 0, sizeof(FlightRecord));
    } else {
        esp_log_level_set("*", ESP_LOG_NONE);   /* Kill logs, no need if we are 
                                                 * in flight. 
                                                 * TODO: this needs to be improved:
                                                 * - kill if we are above boost mode
                                                 * - or ESP_RST_WDT
                                                 * - or ESP_RST_PANIC 
                                                 * - or ESP_RST_BROWNOUT (we can set up, how low the voltage can trigger).
                                                 */
        sys->record->boot_count++;
    }
    /* NOTE: This is nice if we are plugged, but if it fails mid flight then
     * printing all of LOGES/LOGIS is a waster of time. */
    ESP_LOGI(TAG, "╔══════════════════════════════════╗");
    ESP_LOGI(TAG, "║     Star-PI Payload  v2.0        ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════╝");
    
    esp_err_t err;
    sys->health = 0;  /* Everything starts unhealthy */

    /* ── 1. I2C bus (shared by all I2C sensors) ──────────── */
#if CONFIG_ENABLE_I2C_BUS
    err = sys_i2c0_init(MPU6050_SDA, MPU6050_SCL);
    if (ESP_OK == err) {
        sys->health |= 1 << I2C0_HEALTH;
        sys->port.mpu6050 = i2c_bus0_get_handle();
    }

    err = sys_i2c1_init(BME_INA_SDA, BME_INA_SCL);
    if (ESP_OK == err) {
        sys->health |= 1 << I2C1_HEALTH;
        sys->port.bme_ina = i2c_bus1_get_handle();
    }
#endif

#if CONFIG_ENABLE_GPS
    err = sys_uart_init(GPS_UART, GPS_BAUD, GPS_TX, GPS_RX, 0, GPS_RX_BUF_SZ);
    sys->health |= (err == ESP_OK) << GPS_UART_HEALTH;
#endif

#if CONFIG_ENABLE_LORA
    // TODO: define this variables for LoRA
    err = sys_uart_init(LORA_UART, LORA_BAUD, LORA_TX, LORA_RX, LORA_TX_BUF, 0);
    sys->health |= (err == ESP_OK) << LORA_UART_HEALTH;
#endif

#if CONFIG_ENABLE_SD_CARD
    // TODO: maybe this function is not declared anywhere, so we should implement it.
    err = sys_sd_card_init(SD_CLK, SD_CMD, SD_D0);
    sys->health |= (err == ESP_OK) << SD_HEALTH;
#endif

    /* Go/No-Go */
    if (!sys->health) {
        ESP_LOGE(TAG, "Hardware/Driver Dead. Aborting");
        abort();
    }
}



void sys_manager(void *args){
    System *sys = (System *) args;
    uint8_t active_events = 0;

    while(1) {
        xSemaphoreTake(sys->context.manager_up, portMAX_DELAY);

        /* Grab Event */
        taskENTER_CRITICAL(&sys->context.events_guard);
        active_events = sys->context.events;
        sys->context.events = 0; // Reset for the next event
        taskEXIT_CRITICAL(&sys->context.events_guard);

        /* Process Event */
        if (active_events & SHOCK_3G_DETECTED) {
            if (MODE_ARMED == sys->context.mode) {
                sys->context.mode = MODE_BOOST;
            }
        }
        /* if (active_events & ANOTHER_EVENT ) */
    }
}

void sys_POST(System *sys){
    /**
     * Power-On Self-Test
     * ------------------
     *  Bind and ping devices
     */
    sys->context.mode = MODE_POST;
    sys->context.manager_up = xSemaphoreCreateBinary();

    /*-- Initialise Logger buffer --*/
    sys->log_buffer = logger_buff_init();

    /*-- Initialise health monitoring buffer --*/
    sys->hm_buffer = hm_buff_init();
    hm_start_task(sys);
    
    if (sys->health & (1 << I2C0_HEALTH)) {
        if (ESP_OK == mpu6050_init(sys->port.mpu6050)) sys->health |= (1 << MPU6050_HEALTH);
    }

    if (sys->health & (1 << I2C1_HEALTH)) {
#if CONFIG_ENABLE_BME680
        if (ESP_OK == bme680_init(sys->port.bme_ina)) sys->health |= (1 << BME680_HEALTH);
#endif

#if CONFIG_ENABLE_INA219
        if (ESP_OK == ina219_init(sys->port.bme_ina)) sys->health |= (1 << INA219_HEALTH);
#endif
    }

#if CONFIG_ENABLE_GPS
    if (sys->health & (1 << GPS_UART_HEALTH)) {
        if (ESP_OK == gps_init(GPS_UART)) {
            sys->health |= (1 << GPS_HEALTH);
            gps_start_task(sys); // pin to core 1, APP_CPU
            // NOTE: I think GPS lock (numbers of sats connected can be visual things rather than waiting here for connection)
        }
    }
#endif

#if CONFIG_ENABLE_SD_CARD
    if (sys->health & (1 << SD_HEALTH)) {
        // TODO:    frame_logger_init, might need a different name, and it would 
        //          only initiate the file itself where the thing will be stored.
        if (ESP_OK == frame_logger_init()) sys->health |= (1 << FILESYSTEM_HEALTH);
    }
#endif
    /* Go/No-Go */
    // TODO: defined minimun required to flight
    // uint16_t required = (1 << I2C0_HEALTH) | (1 << MPU6050_HEALTH);
    uint16_t required = 0; // whatever we have is enough so far
    if ((sys->health & required) != required) {
        ESP_LOGE(TAG, "POST FATAL : critical hardware missing. Health: 0x%04X.", sys->health);
        /* NOTE: I think if all devices fails it's better to get stuck here in a loop*/
        while(1);
        /* Optionally: we can abort and hope that the reboot fix things, sometimes 
         * from scratch reboot can solve things hehe, in that case, invoke abort().
         * abort(); // reason triggered: ESP_RST_PANIC
         */
    }

}


void app_main(void)
{   
    System sys;
    sys_init(&sys);
    sys_POST(&sys);

    // TODO: SENSOR-CHECK, 
    //      - manual buttom to check if all sensors are ok.
    //      - This only happens when we are doing COLD start
    //      - upon all sensors checked, go armed state
    // TODO: IDLE/ARMED:
    //      - Health monitoring, sending small packets through LoRA every 30 seconds or so
    //      - A failure here triggers a WARM start: 
    //        * sys_init and sys_POST are performed
    //        * SENSOR-CHECK is skipped because it requires a manual thing.
    //      - (opt.) Remote Sensor banning, since we can visually see if the sensor is failing.
    //               This will require parsing data from LoRA (advanced).
    // TODO: BOOST:
    //      - LoRA OFF
    //      - A failure here triggers a HOT start:
    //        * sys_init() 
    //        * sys_POST() might be ingored depending on the failure, if it was a powere failure, then
    // TODO: COAST:
    //      - Change accelerometer range
    // TODO: TURN-OFF PROCEDURE (just if we use NVS). Physical button? command? 

    ESP_LOGI(TAG, "System Health 0x%04X", sys.health);

    if ((sys.health & SENSORS_HEALTH) == 0) {
        ESP_LOGW(TAG, "No sensors enabled! Enable at least one via menuconfig. System Health 0x%04X.", sys.health);
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
    xTaskCreatePinnedToCore(task_sensor_read, "sensor_rd", 4096, (void *)&sys, 6, &sensor_task_h, 1);

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
