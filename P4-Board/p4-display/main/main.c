/**
 * @file main.c
 * @brief Pi-LOG live flight display for the ESP32-P4-Function-EV-Board.
 *
 * Receives the rocket's telemetry and renders it live. Chain end to end:
 *
 *   rocket ESP32-S3 --ESP-NOW--> ground ESP32-C6 --SDIO/esp-hosted--> ESP32-P4
 *
 * The P4 has no radio of its own. The C6 co-processor runs esp-hosted 3.0.9's
 * co-processor firmware plus the ESP-NOW bridge (P4-Board/c6-slave-espnow/cp),
 * which forwards received packets up esp-hosted's peer-data channel under
 * PILOG_MSGID_TELEMETRY. Wi-Fi on the C6 is untouched by that addition, so the
 * P4 keeps network capability alongside this.
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
#include "nvs_flash.h"

#include "bsp/esp-bsp.h"              /* display + touch + LVGL port for this board */
#include "eh_host_core.h"             /* eh_host_init, eh_host_connect_to_slave      */
#include "eh_host_feat_peer_data.h"   /* peer-data channel from the C6               */

#include "telemetry_codec.h"
#include "pilog_link.h"
#include "sd_backup.h"
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

/* ── esp-hosted peer-data callback ────────────────────────────────────── */

/**
 * Called by esp-hosted when the C6 forwards an ESP-NOW payload.
 *
 * Runs in esp-hosted's receive context, which must not be blocked, so this
 * does only the decode - no drawing, no waiting. The decoder is pure
 * computation over a small buffer, which is cheap enough to do inline; the UI
 * task reads the resulting snapshot separately. `data` is only borrowed for
 * the duration of the call; the decoder copies what it keeps.
 */
static void on_peer_data(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)msg_id;
    (void)ctx;
    if (data == NULL || len == 0) return;

    /* Backup first and unconditionally: the card gets every byte, even if the
     * decoder below has to skip this chunk. Never blocks. */
    sd_backup_write(data, len);

    if (xSemaphoreTake(s_codec_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;   /* UI task is mid-read; dropping is better than stalling RPC */
    }
    telemetry_codec_feed(&s_codec, data, len);
    xSemaphoreGive(s_codec_lock);

    s_last_rx_us = esp_timer_get_time();
    s_packets++;
}

/* ── UI refresh task ──────────────────────────────────────────────────── */

#define RATE_WINDOW 3   /* seconds of history behind each displayed rate */

static void ui_task(void *arg)
{
    (void)arg;
    static ui_model_t m;                         /* large: keep it off the stack */
    static uint32_t hist[RATE_WINDOW + 1][TELEM_SENSOR_COUNT];
    static int64_t  hist_us[RATE_WINDOW + 1];
    static uint32_t seen_frames[TELEM_SENSOR_COUNT];
    static int64_t  changed_us[TELEM_SENSOR_COUNT];
    int      hidx = 0;
    int64_t  last_sec_us = esp_timer_get_time();
    uint32_t sec_packets = 0, sec_frames = 0;
    bool     prev_link = false;
    int64_t  link_since_us = last_sec_us;

    for (int i = 0; i <= RATE_WINDOW; i++) hist_us[i] = last_sec_us;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(UI_PERIOD_MS));

        /* Copy the state out under the lock, then render outside it, so the
         * receive path is never blocked by LVGL. */
        if (xSemaphoreTake(s_codec_lock, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        m.st = s_codec.state;
        xSemaphoreGive(s_codec_lock);

        int64_t  now     = esp_timer_get_time();
        uint32_t packets = s_packets;

        m.age_ms  = (s_last_rx_us == 0) ? UINT32_MAX : (uint32_t)((now - s_last_rx_us) / 1000);
        m.link_ok = (s_last_rx_us != 0) && (m.age_ms < LINK_TIMEOUT_MS);
        if (m.link_ok != prev_link) {
            ESP_LOGW(TAG, "link %s (was %s for %.1f s)", m.link_ok ? "UP" : "DOWN",
                     prev_link ? "up" : "down", (double)(now - link_since_us) / 1e6);
            prev_link = m.link_ok;
            link_since_us = now;
        }
        m.link_up_ms = m.link_ok ? (uint32_t)((now - link_since_us) / 1000) : 0;

        /* Liveness: when each sensor last produced a frame, on our clock. */
        for (int s = 0; s < TELEM_SENSOR_COUNT; s++) {
            if (m.st.sensor[s].frames != seen_frames[s]) {
                seen_frames[s]   = m.st.sensor[s].frames;
                changed_us[s]    = now;
                m.sensor_seen[s] = true;
            }
            m.sensor_age_ms[s] = m.sensor_seen[s]
                               ? (uint32_t)((now - changed_us[s]) / 1000) : UINT32_MAX;
        }

        /* Once a second: rates, slow graphs, console diagnostics. */
        m.tick_1hz = (now - last_sec_us >= 1000000);
        if (m.tick_1hz) {
            float dt = (float)(now - last_sec_us) / 1e6f;
            m.pkt_rate   = (packets - sec_packets) / dt;
            m.frame_rate = (m.st.frames_ok - sec_frames) / dt;

            /* Sensor rates over the last RATE_WINDOW seconds: steady for the
             * 1 Hz GPS, still quick to fall to zero when a sensor stops. */
            hidx = (hidx + 1) % (RATE_WINDOW + 1);
            int oldest = (hidx + 1) % (RATE_WINDOW + 1);
            hist_us[hidx] = now;
            for (int s = 0; s < TELEM_SENSOR_COUNT; s++) hist[hidx][s] = m.st.sensor[s].frames;
            float span = (float)(now - hist_us[oldest]) / 1e6f;
            for (int s = 0; s < TELEM_SENSOR_COUNT; s++) {
                m.sensor_hz[s] = (span > 0.5f)
                               ? (hist[hidx][s] - hist[oldest][s]) / span : 0.0f;
            }

            ESP_LOGI(TAG, "link %s  pkts %+lu  frames %+lu  crc-fail %lu  age %lu ms",
                     m.link_ok ? "up" : "DOWN",
                     (unsigned long)(packets - sec_packets),
                     (unsigned long)(m.st.frames_ok - sec_frames),
                     (unsigned long)m.st.frames_crc_fail,
                     (unsigned long)(m.age_ms == UINT32_MAX ? 0 : m.age_ms));
            sec_packets = packets;
            sec_frames  = m.st.frames_ok;
            last_sec_us = now;
        }

        sd_backup_info(&m.sd);

        /* The BSP's LVGL port is not thread-safe; every widget touch must be
         * inside this lock. */
        if (bsp_display_lock(100)) {
            ui_update(&m);
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

    /* UI task before the link: connecting to the C6 includes a reset and a
     * settle delay, and the panel should already show NO LINK meanwhile. */
    if (xTaskCreate(ui_task, "ui", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "ui task create failed");
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init: %s - continuing", esp_err_to_name(err));
    }

    /* Bring up the link to the C6: SDIO transport, RPC, then the peer-data
     * channel. Failures keep the UI alive so they show on the panel rather
     * than only in a log nobody is watching. */
    int rc = eh_host_init(NULL);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "eh_host_init failed: %s", esp_err_to_name(rc));
        return;
    }
    rc = eh_host_connect_to_slave();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "connect to C6 failed: %s - is it running the "
                      "c6-slave-espnow/cp firmware?", esp_err_to_name(rc));
        return;
    }
    err = eh_host_peer_data_register(PILOG_MSGID_TELEMETRY, on_peer_data, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "peer-data registration failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "listening for telemetry (msg id 0x%08lX, ESP-NOW ch %d)",
             (unsigned long)PILOG_MSGID_TELEMETRY, PILOG_ESPNOW_CHANNEL);

    /* Ground-side backup on the P4's own card, if one is inserted. esp-hosted
     * already owns the SDMMC controller by now (card slot 0, C6 slot 1). */
    sd_backup_start(true);
}
