/**
 * @file main.c
 * @brief Pi-LOG live flight display for the ESP32-P4-Function-EV-Board.
 *
 * Receives the rocket's telemetry and renders it live. Chain end to end:
 *
 *   rocket ESP32-S3 --ESP-NOW--> ground ESP32-C6 --SDIO/esp-hosted--> ESP32-P4
 *
 * The P4 has no radio of its own. The C6 co-processor runs the stock esp-hosted
 * slave firmware plus the ESP-NOW bridge from P4-Board/c6-slave-espnow, which
 * forwards received packets up esp-hosted's custom-data channel. Wi-Fi and
 * Bluetooth on the C6 are untouched by that addition, so the P4 keeps full
 * network capability alongside this.
 *
 * Bytes arriving here are the identical framed stream the SD card records, so
 * telemetry_codec decodes them with the same logic as the ground tools.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/esp-bsp.h"          /* display + touch + LVGL port for this board */
#include "esp_hosted.h"           /* esp_hosted_init, custom data channel       */

#include "telemetry_codec.h"
#include "pilog_link.h"
#include "ui.h"

static const char *TAG = "pilog_p4";

/* Link is considered lost after this long without a packet. Generous enough
 * that a couple of dropped chunks do not flash the banner, short enough that a
 * genuine loss is obvious from a distance. */
#define LINK_TIMEOUT_MS   2000
#define UI_PERIOD_MS      100     /* 10 Hz is plenty for a human to read */

static telemetry_codec_t  s_codec;
static SemaphoreHandle_t  s_codec_lock;
static volatile int64_t   s_last_rx_us;
static volatile uint32_t  s_packets;

/* ── esp-hosted custom data callback ──────────────────────────────────── */

/**
 * Called by esp-hosted when the C6 forwards an ESP-NOW payload.
 *
 * Runs in esp-hosted's RPC receive thread, which must not be blocked, so this
 * does only the decode - no drawing, no waiting. The decoder is pure
 * computation over a small buffer, which is cheap enough to do inline; the UI
 * task reads the resulting snapshot separately.
 */
static void on_custom_data(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) return;

    if (xSemaphoreTake(s_codec_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;   /* UI task is mid-read; dropping is better than stalling RPC */
    }
    telemetry_codec_feed(&s_codec, data, len);
    xSemaphoreGive(s_codec_lock);

    s_last_rx_us = esp_timer_get_time();
    s_packets++;
}

/* ── UI refresh task ──────────────────────────────────────────────────── */

static void ui_task(void *arg)
{
    (void)arg;
    telem_state_t snapshot;
    uint32_t last_packets = 0;
    int64_t  last_tick_us = esp_timer_get_time();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(UI_PERIOD_MS));

        /* Copy the state out under the lock, then render outside it, so the
         * receive path is never blocked by LVGL. */
        if (xSemaphoreTake(s_codec_lock, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        snapshot = s_codec.state;
        xSemaphoreGive(s_codec_lock);

        int64_t now = esp_timer_get_time();
        uint32_t packets = s_packets;

        float dt = (float)(now - last_tick_us) / 1000000.0f;
        float rate = (dt > 0.0f) ? (packets - last_packets) / dt : 0.0f;
        last_packets = packets;
        last_tick_us = now;

        uint32_t age_ms = (s_last_rx_us == 0)
                        ? UINT32_MAX
                        : (uint32_t)((now - s_last_rx_us) / 1000);
        bool link_ok = (s_last_rx_us != 0) && (age_ms < LINK_TIMEOUT_MS);

        /* The BSP's LVGL port is not thread-safe; every widget touch must be
         * inside this lock. */
        if (bsp_display_lock(100)) {
            ui_update(&snapshot, link_ok, age_ms, rate);
            bsp_display_unlock();
        }
    }
}

/* ── entry ────────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "Pi-LOG P4 live display starting");

    telemetry_codec_init(&s_codec);
    s_codec_lock = xSemaphoreCreateMutex();
    if (s_codec_lock == NULL) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return;
    }

    /* Display first, so there is something on screen even if the radio path
     * fails - a black panel is indistinguishable from a dead board. */
    bsp_display_start();
    bsp_display_backlight_on();

    if (bsp_display_lock(0)) {
        ui_create();
        bsp_display_unlock();
    }

    /* Bring up the link to the C6. esp_hosted_init() starts the SDIO transport
     * and the RPC layer; Wi-Fi/Bluetooth remain available to this app as usual,
     * the custom-data channel simply runs alongside them. */
    esp_err_t err = esp_hosted_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Is the C6 running compatible slave firmware?");
        /* Keep the UI alive so the failure is visible on the panel rather than
         * only in a log nobody is watching. */
    } else {
        err = esp_hosted_register_rx_callback_custom_data(on_custom_data);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "custom data callback registration failed: %s",
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "listening for telemetry (msg id 0x%08lX, ESP-NOW ch %d)",
                     (unsigned long)PILOG_MSGID_TELEMETRY, PILOG_ESPNOW_CHANNEL);
        }
    }

    if (xTaskCreate(ui_task, "ui", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "ui task create failed");
    }
}
