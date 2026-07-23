
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
#include "driver/gpio.h"

#include "sdkconfig.h"
#include "sensor_config.h"
#include "systemp2i.h"
#include "flight_state.h"
#include "status_led.h"
#include "health_monitoring.h"
#include "flight_stats.h"

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

#if CONFIG_ENABLE_GPS
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
#endif /* CONFIG_ENABLE_GPS */

#if CONFIG_ENABLE_INA219
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
#endif /* CONFIG_ENABLE_INA219 */

#if CONFIG_ENABLE_BME680
void bme680_start_task(System *sys) {
    static TaskHandle_t bme_handle = NULL;
    static struct TaskParams bme_params;
    bme_params.hm_buffer  = sys->hm_buffer;
    bme_params.log_buffer = sys->log_buffer;
    bme_params.context    = &sys->context;
    bme_params.args       = NULL;

    xTaskCreatePinnedToCore(
        bme680_task,
        "bme680_task",
        4096,
        (void *)&bme_params,
        BME680_PRIORITY,
        &bme_handle,
        BME680_CORE);
}
#endif /* CONFIG_ENABLE_BME680 */

#if CONFIG_ENABLE_MPU6050
void mpu6050_start_task(System *sys) {
    static TaskHandle_t mpu6050_handle = NULL;
    static struct TaskParams tparams;

    tparams.hm_buffer = sys->hm_buffer;
    tparams.log_buffer= sys->log_buffer;
    tparams.context = &sys->context;
    tparams.args = (void *)&sys->port.mpu6050;

    xTaskCreatePinnedToCore(
        mpu6050_task,
        "mpu6050_task",
        4096,
        (void *)&tparams,
        MPU6050_PRIORITY,
        &mpu6050_handle,
        MPU6050_CORE);

    mpu6050_start_isr(&mpu6050_handle);
}
#endif /* CONFIG_ENABLE_MPU6050 */

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

#include <errno.h>

void logging_start_task(System *sys) {
    static TaskHandle_t logging_handle = NULL;
    static struct TaskParams params;
    static FILE *f = NULL;

    params.hm_buffer = NULL;
    params.log_buffer = sys->log_buffer;
    params.context = &sys->context;   /* SD writer needs the mode (retain / dump / stream) */
    //params.args = (void *)&sys->open_log_file;
    //params.args = (void *)sys->card;
    params.args = (void *)&sys->sd_ctxt;

    /* Fresh ARMED session -> new log. Resuming BOOST/COAST after an in-flight
     * power cut -> APPEND, so the data recorded before the cut survives. */
    f = NULL;
    if (sys->health & (1 << FILESYSTEM_HEALTH)) {
        const char *open_mode = (flight_state_boot_mode() == MODE_ARMED) ? "wb" : "ab";
        f = fopen(FLIGHT_LOG_FILE_PATH, open_mode);
        if (NULL == f) {
            ESP_LOGE(TAG, "Failed to open %s (%s). err: %s", FLIGHT_LOG_FILE_PATH, open_mode, strerror(errno));
        } else {
            ESP_LOGW(TAG, "flight log %s opened in '%s' mode", FLIGHT_LOG_FILE_PATH, open_mode);
        }
    }
    if (NULL == f) {
        ESP_LOGW(TAG, "no SD log — mirroring the exact SD byte stream over telemetry");
    }
    params.args = (void *)f;

    xTaskCreatePinnedToCore(
        logging_task,
        "logging_task",
        16384,
        (void *)&params,
        LOGGING_PRIORITY,
        &logging_handle,
        LOGGING_CORE
    );

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
            sys_fs_unmount(&sys->card);
        }
        /* if (active_events & ANOTHER_EVENT ) */
    }
}

//--- System Functions --//
// TODO: we might not need this
//#include "esp_pm.h"

void sysP2I_init(System *sys) {
    
    /* TODO: we might not need this
    esp_pm_lock_handle_t pm_lock;
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "i2c_fix", &pm_lock);
    esp_pm_lock_acquire(pm_lock);
    */

    sys->context.mode = MODE_INIT;
    sys->record = &global_flight_record;
    esp_reset_reason_t reason = esp_reset_reason();

    if (reason == ESP_RST_POWERON) {
        memset(sys->record, 0, sizeof(FlightRecord));
    } else {
        /* BENCH TEST: log-kill disabled so serial works on USB resets. On the S3,
         * flashing/monitoring triggers USB_UART_CHIP_RESET (not ESP_RST_POWERON),
         * which would otherwise silence every log below. Restore the
         * esp_log_level_set("*", ESP_LOG_NONE) call for flight. */
        /* Kill logs, no need if we are
                                                 * in flight. 
                                                 * TODO: this needs to be improved:
                                                 * - kill if we are above boost mode
                                                 * - or ESP_RST_WDT
                                                 * - or ESP_RST_PANIC 
                                                 * - or ESP_RST_BROWNOUT (we can set up, how low the voltage can trigger).
                                                 */
        sys->record->boot_count++;
    }

    /* Telemetry transport — wired UART now (BLE/ESP-NOW relay later). Init on
     * every reset reason; on this target the "BT" bridge is a no-RF UART. */
    bt_serial_init("StarPi-Telemetry");

    ESP_LOGI(TAG, "╔══════════════════════════════════╗");
    ESP_LOGI(TAG, "║     Star-PI Payload  v2.0        ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════╝");
    
    esp_err_t err;
    sys->health = 0;  /* Everything starts unhealthy */

    /* ── 1. I2C bus (shared by all I2C sensors) ──────────── */
#if CONFIG_ENABLE_I2C_BUS
    err = sys_i2c_init(MPU6050_I2C, MPU6050_SDA, MPU6050_SCL);
    if (ESP_OK == err) {
        sys->health |= 1 << I2C0_HEALTH;
        sys->port.mpu6050 = i2c_bus0_get_handle();
    }

    err = sys_i2c_init(BME_INA_I2C, BME_INA_SDA, BME_INA_SCL);
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

    /*-- Status RGB LED: shows the flight mode (blue POST / green ARMED / red BOOST / amber COAST) --*/
    status_led_init();
    status_led_start_task(&sys->context);
    
#if CONFIG_ENABLE_MPU6050
    /* ── Phase 1: initialise every sensor — NO tasks yet, so the calibration
     * frame below is guaranteed to be the FIRST telemetry frame of the boot. */
    if (sys->health & (1 << I2C0_HEALTH)) {
        if (ESP_OK == mpu6050_init(sys->port.mpu6050)) {
            sys->health |= (1 << MPU6050_HEALTH);
        }
    }
#endif

    if (sys->health & (1 << I2C1_HEALTH)) {
#if CONFIG_ENABLE_BME680
        if (ESP_OK == bme680_init(sys->port.bme_ina)) {
            sys->health |= (1 << BME680_HEALTH);
        }
#endif

#if CONFIG_ENABLE_INA219
        if (ESP_OK == ina219_init(sys->port.bme_ina)) {
            sys->health |= (1 << INA219_HEALTH);
        }
#endif
    }

#if CONFIG_ENABLE_GPS
    if (sys->health & (1 << GPS_UART_HEALTH)) {
        /* This SAM-M10Q board runs at 9600 baud (GPS_BAUD); no baud upgrade. */
        if (ESP_OK == gps_probe(GPS_UART)) {
            sys->health |= (1 << GPS_HEALTH);
            gps_disable_non_essential_NMEA(GPS_UART);
        } else {
            ESP_LOGE(TAG, "GPS probe failed at %d baud.", GPS_BAUD);
        }
    }
#endif

    /* ── Phase 2: calibration frame ───────────────────────────
     * Raw sensor constants for ground-side conversion: sent over telemetry NOW
     * (before any sensor task runs, so it is the first frame of every boot /
     * re-arm) and written at the head of every SD log session by the logging
     * task. All logged sensor frames are RAW; the ground applies the math. */
    {
        static struct calib_frame_v1 calib = {
            .version              = 1,
            .mpu_accel_fs_g_armed = 2,
            .mpu_accel_fs_g_boost = 16,
            .mpu_gyro_fs_dps      = 500,
            .ina_shunt_mohm       = 100,
        };
#if CONFIG_ENABLE_BME680
        if (sys->health & (1 << BME680_HEALTH)) {
            uint16_t blen = 0;
            const uint8_t *blob = bme680_get_calib_blob(&blen);
            if (blob && blen == sizeof(calib.bme680)) {
                memcpy(calib.bme680, blob, blen);
            }
        }
#endif
        logging_set_calib(&calib, sizeof(calib));                       /* -> SD session head */
        hm_send(sys->hm_buffer, SBIT_CALIB, (uint8_t *)&calib, sizeof(calib));  /* -> telemetry */
        ESP_LOGI(TAG, "calibration frame ready (%u B)", (unsigned)sizeof(calib));
    }

    /* ── Phase 3: start the sensor tasks (calib frame is already out) ── */
#if CONFIG_ENABLE_MPU6050
    if (sys->health & (1 << MPU6050_HEALTH)) mpu6050_start_task(sys);
#endif
#if CONFIG_ENABLE_BME680
    if (sys->health & (1 << BME680_HEALTH))  bme680_start_task(sys);
#endif
#if CONFIG_ENABLE_INA219
    if (sys->health & (1 << INA219_HEALTH))  ina219_start_task(sys);
#endif
#if CONFIG_ENABLE_GPS
    if (sys->health & (1 << GPS_HEALTH))     gps_start_task(sys);
#endif

#if CONFIG_ENABLE_SD_SPI
    if (sys->health & (1 << SD_HEALTH)) {
        sys->sd_ctxt.card = & sys->card;
        if (ESP_OK == sys_mount_spi_card(SD_PORT, SD_MOUNT_POINT, &sys->sd_ctxt.card)) {
            sys->health |= (1 << FILESYSTEM_HEALTH);
        }
        /*
        if (ESP_OK == sys_sd_init(SD_PORT, &sys->sd_ctxt.card, &sys->sd_ctxt.starting_sector)) {
            sys->health |= (1 << FILESYSTEM_HEALTH);
            // logging_start_task(sys);
            if (ESP_OK == logging_init(&sys->open_log_file, FLIGHT_LOG_FILE_PATH)) {
                sys->health |= (1 << LOG_FILE_HEALTH);
                logging_start_task(sys);
            }
        }
        */
    }
#endif

    /* SD writer: drains the ring to /sd/fly.bin when a card is mounted. With no
     * card (or a failed mount) it mirrors the IDENTICAL byte stream over the
     * telemetry UART, so the PC can capture exactly what the SD would receive
     * — including the calibration head, the ARMED sparse record, the
     * pre-trigger dump on launch and the full-rate flight stream. */
    logging_start_task(sys);

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

    /* Resolve the boot flight-mode FIRST (NVS restore / BOOT-hold re-arm):
     * the SD logger in POST needs it to pick append vs fresh log. */
    flight_state_preinit();

    sysP2I_init(&sys);
    sysP2I_POST(&sys);

    ESP_LOGI(TAG, "System Health 0x%04X", sys.health);

    if ((sys.health & SENSORS_HEALTH) == 0) {
        ESP_LOGW(TAG, "No sensors enabled! Enable at least one via menuconfig. System Health 0x%04X.", sys.health);
    }


    /* Live rate monitor: logs achieved samples/sec per sensor once a second. */
    xTaskCreatePinnedToCore(flight_stats_task, "rates", 3072,
                            (void *)&sys.context, 1, NULL, 0);

    /* Flight state machine: restore the saved mode from NVS (or re-arm if BOOT is
     * held ~1.5 s at boot), then the MPU task drives ARMED -> BOOST -> COAST. */
    flight_state_init(&sys.context);
    ESP_LOGW(TAG, "flight mode = %s  (BOOST at >= %.2f g; hold BOOT ~%d ms at boot to re-arm)",
             sys.context.mode == MODE_ARMED ? "ARMED" :
             sys.context.mode == MODE_BOOST ? "BOOST" :
             sys.context.mode == MODE_COAST ? "COAST" : "?",
             (double)FLIGHT_LAUNCH_TRIP_G, FLIGHT_REARM_HOLD_MS);

    /* Idle heartbeat. Re-sends the calibration frame over telemetry every ~30 s
     * while on the pad, so a ground station attached late still receives the
     * constants it needs to convert the raw sensor frames. */
    int hb = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (++hb >= 6) {
            hb = 0;
            uint32_t m = sys.context.mode;
            if (m == MODE_POST || m == MODE_ARMED) {
                const void *cp = NULL;
                uint16_t cl = logging_get_calib(&cp);
                if (cl) hm_send(sys.hm_buffer, SBIT_CALIB, (uint8_t *)cp, cl);
            }
        }
    }
}
