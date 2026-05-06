
/**
 * @file main.c
 * @brief Star-PI Payload — modular sensor orchestrator.
 *
 * Enable / disable peripherals via  `idf.py menuconfig`
 *   → "Star-PI Payload Configuration"
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
#include "sdif.h"

static const char *TAG = "main";

/* Global Variables */
RTC_DATA_ATTR FlightRecord global_flight_record;

#if CONFIG_ENABLE_SD_CARD
#endif 

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
        GPS_PRIORITY,
        &gps_handler, 
        GPS_CORE
    );

    // TODO: link this taks to system
    // sys->tasks.gps = &gps_handler;
}

void ina219_start_task(System *sys) {
    static TaskHandle_t ina219_handler = NULL;
    static struct TaskParams ina219_params;

    ina219_params.hm_buffer = sys->hm_buffer;
    ina219_params.log_buffer = sys->log_buffer;
    ina219_params.context = &sys->context;

    xTaskCreatePinnedToCore(
        ina219_task,
        "ina219_task",
        4096,
        (void *) &ina219_params,
        INA219_PRIORITY, 
        &ina219_handler,
        INA219_CORE
    );

    // TODO: link this task to the system:
    // sys->tasks.ina216 = ina219_handler;
}

#include "esp_timer.h" // For microsecond-accurate timing
#include <fcntl.h>
#include <unistd.h>
#include "esp_heap_caps.h"

void sd_fat32_benchmark_task(void *pvParameters) {
    ESP_LOGI("SD-FAT32-BENCHMARK", "Starting POSIX SD Card Write Speed Test...");

    const size_t buffer_size = 4096;
    uint8_t *dummy_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA);
    if (dummy_buffer == NULL) {
        ESP_LOGE("BENCHMARK", "Failed to allocate buffer!");
        vTaskDelete(NULL);
        return;
    }
    memset(dummy_buffer, 0xAA, buffer_size);

    int fd = open("/sd/bench.bin", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    
    if (fd < 0) { // POSIX returns -1 on error, not NULL
        ESP_LOGE("BENCHMARK", "Failed to open file!");
        free(dummy_buffer);
        vTaskDelete(NULL);
        return;
    }

    const int iterations = 1000;
    int successful_writes = 0;

    ESP_LOGI("BENCHMARK", "Writing %d MB of data...", (iterations * buffer_size) / (1024 * 1024));

    int64_t start_time_us = esp_timer_get_time();

    for (int i = 0; i < iterations; i++) {
        int64_t t1 = esp_timer_get_time();
        ssize_t written = write(fd, dummy_buffer, buffer_size);
        int64_t t2 = esp_timer_get_time();
        fsync(fd);
        int64_t t3 = esp_timer_get_time();
        printf("Write took: %lld us | fsync took: %lld us\n", (t2 - t1), (t3 - t2));
        
        if (written == buffer_size) {
            successful_writes++;
        } else {
            ESP_LOGE("BENCHMARK", "Write failed at iteration %d! Error code: %d", i, written);
            break; // Stop if the hardware fails
        }
    }

    int64_t end_time_us = esp_timer_get_time();

    // 3. Use POSIX close()
    close(fd);
    free(dummy_buffer);

    int64_t time_taken_us = end_time_us - start_time_us;
    float time_taken_sec = (float)time_taken_us / 1000000.0f;
    uint32_t total_bytes_written = successful_writes * buffer_size;
    float speed_kbs = (total_bytes_written / 1024.0f) / time_taken_sec;

    ESP_LOGI("BENCHMARK", "=== RESULTS ===");
    ESP_LOGI("BENCHMARK", "Speed: %.2f KB/s", speed_kbs);
    ESP_LOGI("BENCHMARK", "===============");

    vTaskDelete(NULL);
}

void sd_raw_benchmark_task(void *params) {
    ESP_LOGI("SD-RAW-BENCHMARK", "Starting POSIX SD Card Write Speed Test...");
    struct TaskParams *t_params = (struct TaskParams *)params;
    struct SDContext *sd = (struct SDContext *)t_params->args;
    const size_t buffer_size = 16384;
    size_t sector_count = buffer_size / 512;
    esp_err_t ret;
    uint8_t *dummy_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA);
    if (dummy_buffer == NULL) {
        ESP_LOGE("SD-RAW-BENCHMARK", "Failed to allocate buffer!");
        vTaskDelete(NULL);
        return;
    }
    memset(dummy_buffer, 0xAA, buffer_size);

    const int iterations = 1000;
    int successful_writes = 0;

    ESP_LOGI("SD-RAW-BENCHMARK", "Writing %d MB of data...", (iterations * buffer_size) / (1024 * 1024));

    int64_t start_time_us = esp_timer_get_time();

    for (int i = 0; i < iterations; i++) {
        int64_t t1 = esp_timer_get_time();
        ret = sdmmc_write_sectors(
            &sd->card, 
            dummy_buffer, 
            sd->starting_sector, 
            sector_count);
        sd->starting_sector += sector_count;
        int64_t t2 = esp_timer_get_time();
        printf("Write took: %lld us \n", (t2 - t1));
        if (ESP_OK == ret) {
            successful_writes++;
        } else {
            ESP_LOGE("SD-RAW-BENCHMARK", "Write failed at iteration %d! Error: %s", i, esp_err_to_name(ret));
            break; // Stop if the hardware fails
        }

        //ret = spi_sd_pre_erase(&sd->card, sector_count);
        //if (ret != ESP_OK) {
        //    ESP_LOGE("SD-RAW-BENCHMARK", "something went wrong with pre-allocation: %s", esp_err_to_name(ret));
        //}
    }

    int64_t end_time_us = esp_timer_get_time();

    heap_caps_free(dummy_buffer);

    int64_t time_taken_us = end_time_us - start_time_us;
    float time_taken_sec = (float)time_taken_us / 1000000.0f;
    uint32_t total_bytes_written = successful_writes * buffer_size;
    float speed_kbs = (total_bytes_written / 1024.0f) / time_taken_sec;

    ESP_LOGI("BENCHMARK", "=== RESULTS ===");
    ESP_LOGI("BENCHMARK", "Speed: %.2f KB/s", speed_kbs);
    ESP_LOGI("BENCHMARK", "===============");

    vTaskDelete(NULL);
}

void logging_start_task(System *sys) {
    // TODO:
    // do not open the run the task yet, because the file is open.
    // test writing the into the sd card here. several times until 
    // the sdcards stops.
    // we need to close the file, because file is open
    static TaskHandle_t logging_handle = NULL;
    static struct TaskParams params;

    params.hm_buffer = NULL;
    params.log_buffer = sys->log_buffer;
    params.context = NULL;
    //params.args = (void *)&sys->open_log_file;
    //params.args = (void *)sys->card;
    params.args = (void *)&sys->sd_ctxt;
    
    /*
    setvbuf(sys->open_log_file, NULL, _IONBF, 0);
    fprintf(sys->open_log_file, "Test IONBF");

    const size_t buffer_size = 512;
    uint8_t *dummy_buffer = malloc(buffer_size);
    if (dummy_buffer == NULL) {
        ESP_LOGE("BENCHMARK", "Failed to allocate buffer!");
        vTaskDelete(NULL);
        return;
    }
    memset(dummy_buffer, 0xAA, buffer_size);

    */
    close(sys->open_log_file);
    /*
    sys_fs_unmount(sys->card);
    */
    
    
    xTaskCreatePinnedToCore(
        sd_raw_benchmark_task,
        "sd_benchmark_task",
        16384,
        (void *)&params,
        LOGGING_PRIORITY,
        &logging_handle,
        LOGGING_CORE
    );

    /*
    xTaskCreatePinnedToCore(
        logging_task,
        "logging_task",
        4096,
        (void *)&params,
        LOGGING_PRIORITY,
        &logging_handle,
        LOGGING_CORE
    );
    */

    // TODO: link this task to the system:
    // sys->tasks.logging = logging_handle;
}

void sys_manager(void *args) {
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
        if (active_events & EVT_SHOCK_3G_DETECTED) {
            if (MODE_ARMED == sys->context.mode) {
                sys->context.mode = MODE_BOOST;
            }
        }
        if (active_events & EVT_TERMINATE_LOG) {
            // TODO: this event doesn't exist anymore
            ESP_LOGI("MAIN", "Umount fs.");
            close(sys->open_log_file);
            sys_fs_unmount(sys->card);
        }
        /* if (active_events & ANOTHER_EVENT ) */
    }
}

//--- System Functions --//

void sysP2I_init(System *sys) {
    sys->context.mode = MODE_INIT;
    sys->record = &global_flight_record;
    esp_reset_reason_t reason = esp_reset_reason();

    if (reason == ESP_RST_POWERON) {
        memset(sys->record, 0, sizeof(FlightRecord));
        bt_serial_init("GPS-Serial-Bluetooth");
        vTaskDelay(pdMS_TO_TICKS(500)); // Compulsory attached to BLT initialization, 
                                        // otherwise RF might affect measurments.
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

#if CONFIG_ENABLE_SD_SPI
    // TODO: maybe this function is not declared anywhere, so we should implement it.
    err = sys_hardware_sd_init(SD_PORT);
    sys->health |= (err == ESP_OK) << SD_HEALTH;
#endif

    /* Go/No-Go */
    if (!sys->health) {
        ESP_LOGE(TAG, "Hardware/Driver Dead. Aborting");
        abort();
    }
}



void sysP2I_POST(System *sys){
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
        if (ESP_OK == bme680_init(sys->port.bme_ina)) {
            sys->health |= (1 << BME680_HEALTH);
        }
#endif

#if CONFIG_ENABLE_INA219
        if (ESP_OK == ina219_init(sys->port.bme_ina)) {
            sys->health |= (1 << INA219_HEALTH);
            ina219_start_task(sys);
        }
#endif
    }

#if CONFIG_ENABLE_GPS
    if (sys->health & (1 << GPS_UART_HEALTH)) {
        if (ESP_OK == gps_probe(GPS_UART)) {
            ESP_LOGI(TAG, "GPS found at 9600, upgrading...");
            gps_upgrade_baud_115200(GPS_UART);
        }
        if (ESP_OK == sys_uart_baud(GPS_UART, GPS_UART_BAUD115200)) {
            sys_uart_flush(GPS_UART);
            if (ESP_OK == gps_probe(GPS_UART)) {
                sys->health |= (1 << GPS_HEALTH);
                gps_disable_non_essential_NMEA(GPS_UART);
                gps_start_task(sys);
            } else {
                ESP_LOGE(TAG, "GPS probe failed at 115200.");
            }
        } else {
            ESP_LOGE(TAG, "Couldn't upgrade ESP's for GPS.");
            sys->health &= ~(1 << GPS_UART_HEALTH);
        }
    }
#endif

#if CONFIG_ENABLE_SD_SPI
    if (sys->health & (1 << SD_HEALTH)) {
        if (ESP_OK == sys_sd_init(SD_PORT, &sys->sd_ctxt.card, &sys->sd_ctxt.starting_sector)) {
            sys->health |= (1 << FILESYSTEM_HEALTH);
            // logging_start_task(sys);
            /*
            if (ESP_OK == logging_init(&sys->open_log_file, FLIGHT_LOG_FILE_PATH)) {
                sys->health |= (1 << LOG_FILE_HEALTH);
                logging_start_task(sys);
            }
            */
        }
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
    static System sys;
    sysP2I_init(&sys);
    sysP2I_POST(&sys);

    // NOTE: SENSOR-CHECK, 
    //      - This is a visual inspection procedure.
    // TODO: IDLE/ARMED:
    //      - Health monitoring, sending small packets through LoRA every 30 seconds or so
    //      - A failure here triggers a WARM start: 
    //      - (opt.) Remote Sensor banning, since we can visually see if the sensor is failing.
    //               This will require parsing data from LoRA (advanced).
    // TODO: BOOST:
    //      - BLT OFF
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

    /* ── 6. Heartbeat / watchdog loop ────────────────────── */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

}
