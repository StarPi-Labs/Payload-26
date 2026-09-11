/**
 * @file telemetry_espnow.c
 * @brief ESP-NOW transport - implementation.
 *
 * Design mirrors telemetry_net.c deliberately: send() copies into a ring and
 * returns, a low-priority task drains it, and a full ring drops whole chunks
 * rather than stalling the logger. The SD card is the flight recorder; the
 * radio is best-effort.
 *
 * ESP-NOW specifics that shape this file:
 *
 *  - Payload cap is 250 bytes (PILOG_ESPNOW_CHUNK leaves room for our header),
 *    versus 1400 for the UDP transport. More packets, same total bytes.
 *  - Broadcast is unacknowledged. esp_now_send() returning ESP_OK only means
 *    the frame was queued to the radio, never that anyone heard it.
 *  - The radio will not accept a new frame while the previous one is still in
 *    flight, so the drain task paces itself rather than spinning on ESP_ERR_
 *    ESPNOW_NO_MEM.
 */

#include "telemetry_espnow.h"
#include "sdkconfig.h"

#if CONFIG_ENABLE_TELEMETRY_ESPNOW

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "pilog_link.h"

static const char *TAG = "telem_now";

#define RING_CAP     (16 * 1024)
#define TX_IDLE_MS   20
#define LOCK_WAIT_MS 2

static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static uint8_t           s_ring[RING_CAP];
static size_t            s_head, s_tail, s_used;
static SemaphoreHandle_t s_lock;
static volatile bool     s_started;
static volatile uint32_t s_dropped, s_sent, s_seq;

/* ── Ring buffer (caller holds s_lock) ────────────────────────────────── */

/* All-or-nothing: a chunk is never half-written, so the receiver never sees a
 * frame spliced together from two different points in the stream. */
static bool ring_put(const uint8_t *src, size_t len)
{
    if (len > RING_CAP - s_used) return false;
    size_t first = RING_CAP - s_head;
    if (first > len) first = len;
    memcpy(s_ring + s_head, src, first);
    if (len > first) memcpy(s_ring, src + first, len - first);
    s_head = (s_head + len) % RING_CAP;
    s_used += len;
    return true;
}

static size_t ring_get(uint8_t *dst, size_t max)
{
    size_t len = (s_used < max) ? s_used : max;
    if (len == 0) return 0;
    size_t first = RING_CAP - s_tail;
    if (first > len) first = len;
    memcpy(dst, s_ring + s_tail, first);
    if (len > first) memcpy(dst + first, s_ring, len - first);
    s_tail = (s_tail + len) % RING_CAP;
    s_used -= len;
    return len;
}

/* ── Drain task ───────────────────────────────────────────────────────── */

static void telemetry_espnow_task(void *arg)
{
    (void)arg;
    /* Header immediately followed by payload in one buffer, so the whole chunk
     * goes out as a single ESP-NOW frame. */
    static uint8_t frame[sizeof(pilog_chunk_hdr_t) + PILOG_ESPNOW_CHUNK];
    pilog_chunk_hdr_t *hdr = (pilog_chunk_hdr_t *)frame;
    uint8_t *body = frame + sizeof(pilog_chunk_hdr_t);

    while (1) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t n = ring_get(body, PILOG_ESPNOW_CHUNK);
        xSemaphoreGive(s_lock);

        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(TX_IDLE_MS));
            continue;
        }

        hdr->seq       = s_seq;
        hdr->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);

        esp_err_t err = esp_now_send(BROADCAST_MAC, frame, sizeof(pilog_chunk_hdr_t) + n);
        if (err == ESP_OK) {
            s_seq++;
            s_sent++;
        } else if (err == ESP_ERR_ESPNOW_NO_MEM) {
            /* Radio queue full: the previous frame is still going out. Back off
             * briefly rather than burning CPU retrying. The bytes are already
             * out of the ring, so this chunk is lost - acceptable, and the
             * receiver resyncs on the next sync word. */
            vTaskDelay(pdMS_TO_TICKS(5));
        } else if ((s_dropped++ % 100) == 0) {
            ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(err));
        }

        /* Pace slightly even on success: back-to-back 230 B broadcasts saturate
         * the radio and starve everything else on the chip. */
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

bool telemetry_espnow_init(void)
{
    if (s_started) return true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s - link disabled", esp_err_to_name(err));
        return false;
    }

    /* Tolerate these already existing: telemetry_net.c may have brought the
     * stack up first, and both transports can run together. */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init failed: %s - link disabled", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop failed: %s - link disabled", esp_err_to_name(err));
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "wifi init failed: %s - link disabled", esp_err_to_name(err));
        return false;
    }

    /* If telemetry_net.c already put us in AP mode, leave that alone - ESP-NOW
     * works from any started mode. Only force STA when nothing has set a mode. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_NULL) {
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
            ESP_LOGE(TAG, "set_mode failed - link disabled");
            return false;
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STOPPED) {
        ESP_LOGE(TAG, "wifi start failed: %s - link disabled", esp_err_to_name(err));
        return false;
    }

    /* Only pin the channel when we own the radio. If a SoftAP is already up,
     * changing channel underneath it would drop its clients. */
    if (mode == WIFI_MODE_NULL) {
        esp_wifi_set_channel(PILOG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    }
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed - link disabled");
        return false;
    }

    esp_now_peer_info_t peer = {
        .channel = 0,               /* 0 = whatever channel we are on */
        .ifidx   = (mode == WIFI_MODE_AP) ? WIFI_IF_AP : WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, BROADCAST_MAC, ESP_NOW_ETH_ALEN);
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "add_peer failed: %s - link disabled", esp_err_to_name(err));
        return false;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "mutex alloc failed - link disabled");
        return false;
    }

    /* Low priority: sensor sampling and SD writes must always win. */
    if (xTaskCreate(telemetry_espnow_task, "telem_now", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed - link disabled");
        return false;
    }

    s_started = true;
    ESP_LOGI(TAG, "ESP-NOW broadcasting on channel %d, %d B chunks",
             PILOG_ESPNOW_CHANNEL, PILOG_ESPNOW_CHUNK);
    return true;
}

void telemetry_espnow_send(const uint8_t *data, size_t len)
{
    if (!s_started || data == NULL || len == 0) return;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOCK_WAIT_MS)) != pdTRUE) {
        s_dropped++;
        return;
    }
    bool queued = ring_put(data, len);
    xSemaphoreGive(s_lock);

    if (!queued && (++s_dropped % 100) == 1) {
        ESP_LOGW(TAG, "link saturated - %lu chunks dropped so far",
                 (unsigned long)s_dropped);
    }
}

uint32_t telemetry_espnow_dropped(void) { return s_dropped; }
uint32_t telemetry_espnow_sent(void)    { return s_sent; }

#else /* !CONFIG_ENABLE_TELEMETRY_ESPNOW */

bool     telemetry_espnow_init(void)                       { return false; }
void     telemetry_espnow_send(const uint8_t *d, size_t l) { (void)d; (void)l; }
uint32_t telemetry_espnow_dropped(void)                    { return 0; }
uint32_t telemetry_espnow_sent(void)                       { return 0; }

#endif /* CONFIG_ENABLE_TELEMETRY_ESPNOW */
