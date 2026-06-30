/**
 * @file flight_stats.c
 * @brief Live per-sensor sample-rate monitor (see flight_stats.h).
 */

#include "flight_stats.h"
#include "systemp2i.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

volatile uint32_t g_flight_stats[STAT_COUNT] = {0};

static const char *TAG = "rates";

void flight_stats_task(void *arg)
{
    struct SysContext *ctx = (struct SysContext *)arg;
    static const char *mode_name[] = {
        "INIT", "POST", "SNSCHK", "ARMED", "BOOST", "COAST"
    };

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* Snapshot + reset; the 1 s window makes each count == Hz directly. */
        uint32_t mpu = g_flight_stats[STAT_MPU6050]; g_flight_stats[STAT_MPU6050] = 0;
        uint32_t ina = g_flight_stats[STAT_INA219];  g_flight_stats[STAT_INA219]  = 0;
        uint32_t bme = g_flight_stats[STAT_BME680];  g_flight_stats[STAT_BME680]  = 0;
        uint32_t gps = g_flight_stats[STAT_GPS];     g_flight_stats[STAT_GPS]     = 0;

        uint32_t m = ctx ? ctx->mode : 0;
        ESP_LOGI(TAG, "[%-5s] mpu=%-3lu ina=%-2lu bme=%-3lu gps=%lu  (Hz)",
                 (m < 6) ? mode_name[m] : "?",
                 (unsigned long)mpu, (unsigned long)ina,
                 (unsigned long)bme, (unsigned long)gps);
    }
}
