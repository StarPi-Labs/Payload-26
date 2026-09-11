/**
 * @file espnow_bridge.c
 * @brief ESP-NOW receiver bolted onto the esp-hosted slave firmware.
 *
 * Data path:
 *
 *   rocket S3  --ESP-NOW-->  C6 recv callback  -->  queue  -->  forward task
 *                                                                    |
 *                              esp_hosted_send_custom_data(PILOG_MSGID_TELEMETRY)
 *                                                                    |
 *                                                            SDIO --> P4 host
 *
 * Two rules drive the shape of this file:
 *
 *  1. The ESP-NOW receive callback runs in Wi-Fi task context. Espressif's
 *     documentation is explicit that it must not block. So it does nothing but
 *     copy into a queue with a zero timeout - anything slower would stall the
 *     Wi-Fi driver and, because this is the same radio esp-hosted serves the
 *     P4 with, would degrade the host's Wi-Fi too.
 *
 *  2. esp_hosted_send_custom_data() talks to the host over SDIO and can block.
 *     It is therefore called only from our own task, never from the callback.
 *
 * Nothing here touches the slave's existing behaviour. ESP-NOW coexists with
 * station and SoftAP mode on one radio, so Wi-Fi and Bluetooth for the P4 carry
 * on untouched - the one constraint is that ESP-NOW follows whatever channel
 * the Wi-Fi interface is on (see PILOG_ESPNOW_CHANNEL).
 */

#include "espnow_bridge.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"

#include "esp_hosted_peer_data.h"   /* esp_hosted_send_custom_data() */
#include "pilog_link.h"

static const char *TAG = "espnow_br";

/* One ESP-NOW frame is at most 250 B. Queue depth trades RAM for tolerance of
 * a momentarily busy SDIO link; 24 is a few hundred ms of margin at the
 * rocket's chunk rate. */
#define FWD_QUEUE_DEPTH   24
#define FWD_TASK_STACK    4096
#define FWD_TASK_PRIO     5

typedef struct {
    uint16_t len;
    uint8_t  data[ESP_NOW_MAX_DATA_LEN];
} fwd_item_t;

static QueueHandle_t     s_queue;
static volatile bool     s_started;
static volatile uint32_t s_received, s_dropped, s_forwarded, s_fwd_errors;
static volatile uint32_t s_last_seq;
static volatile bool     s_have_seq;
static volatile uint32_t s_gaps;

/* ── ESP-NOW receive callback (Wi-Fi task context - must not block) ────── */

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    (void)info;

    /* Ignore anything too short to carry our header - other ESP-NOW traffic in
     * the area, or a truncated frame. */
    if (len <= (int)sizeof(pilog_chunk_hdr_t) || len > ESP_NOW_MAX_DATA_LEN) {
        return;
    }

    /* Track chunk-sequence gaps for link diagnostics. We deliberately do not
     * try to repair them: every telemetry frame carries its own sync word and
     * CRC, so the decoder on the P4 resynchronises by itself. */
    pilog_chunk_hdr_t hdr;
    memcpy(&hdr, data, sizeof(hdr));
    if (s_have_seq && hdr.seq > s_last_seq + 1) {
        s_gaps += hdr.seq - s_last_seq - 1;
    }
    s_last_seq = hdr.seq;
    s_have_seq = true;
    s_received++;

    static fwd_item_t item;   /* callback is not re-entrant; avoids a big stack */
    item.len = (uint16_t)(len - sizeof(pilog_chunk_hdr_t));
    memcpy(item.data, data + sizeof(pilog_chunk_hdr_t), item.len);

    /* Zero timeout: never block the Wi-Fi task. A full queue means the host
     * link cannot keep up, and dropping is the correct response. */
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        s_dropped++;
    }
}

/* ── Forwarding task (may block on SDIO) ──────────────────────────────── */

static void espnow_forward_task(void *arg)
{
    (void)arg;
    static fwd_item_t item;
    uint32_t last_report = 0;

    while (1) {
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;   /* idle - loop so the stats log below still ticks */
        }

        esp_err_t err = esp_hosted_send_custom_data(PILOG_MSGID_TELEMETRY,
                                                    item.data, item.len);
        if (err == ESP_OK) {
            s_forwarded++;
        } else if ((s_fwd_errors++ % 50) == 0) {
            ESP_LOGW(TAG, "send_custom_data failed: %s", esp_err_to_name(err));
        }

        /* Periodic health line, cheap enough to leave in. Silence here while
         * the rocket is powered means the ESP-NOW side is the problem; errors
         * here mean the SDIO side is. */
        if (s_forwarded - last_report >= 200) {
            last_report = s_forwarded;
            ESP_LOGI(TAG, "rx %lu  fwd %lu  dropped %lu  seq-gaps %lu",
                     (unsigned long)s_received, (unsigned long)s_forwarded,
                     (unsigned long)s_dropped,  (unsigned long)s_gaps);
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

bool espnow_bridge_init(void)
{
    if (s_started) return true;

    /* The slave firmware owns Wi-Fi init; we only require that it has started.
     * esp_now_init() fails cleanly if it has not, so this is a clear error
     * rather than a subtle malfunction. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
        ESP_LOGE(TAG, "Wi-Fi not started yet - call espnow_bridge_init() after "
                      "the slave has brought Wi-Fi up");
        return false;
    }

    s_queue = xQueueCreate(FWD_QUEUE_DEPTH, sizeof(fwd_item_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "queue alloc failed - ESP-NOW bridge disabled");
        return false;
    }

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s - bridge disabled", esp_err_to_name(err));
        vQueueDelete(s_queue);
        s_queue = NULL;
        return false;
    }

    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_recv_cb failed: %s - bridge disabled", esp_err_to_name(err));
        esp_now_deinit();
        vQueueDelete(s_queue);
        s_queue = NULL;
        return false;
    }

    if (xTaskCreate(espnow_forward_task, "espnow_fwd", FWD_TASK_STACK,
                    NULL, FWD_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed - bridge disabled");
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        vQueueDelete(s_queue);
        s_queue = NULL;
        return false;
    }

    /* Only pin the channel when nothing else is dictating it. If the slave has
     * associated with an access point on the host's behalf, that association
     * owns the channel and forcing ours would break the host's Wi-Fi - the
     * rocket has to follow the AP's channel in that case instead. */
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&primary, &second);

    if (mode == WIFI_MODE_STA) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
            /* Not associated - safe to choose our own channel. */
            esp_wifi_set_channel(PILOG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
            primary = PILOG_ESPNOW_CHANNEL;
        }
    }

    s_started = true;
    ESP_LOGI(TAG, "ESP-NOW bridge up on channel %u (expected %d), msg id 0x%08lX",
             (unsigned)primary, PILOG_ESPNOW_CHANNEL,
             (unsigned long)PILOG_MSGID_TELEMETRY);
    if (primary != PILOG_ESPNOW_CHANNEL) {
        ESP_LOGW(TAG, "channel %u != %d - the rocket must transmit on %u or "
                      "nothing will be received",
                 (unsigned)primary, PILOG_ESPNOW_CHANNEL, (unsigned)primary);
    }
    return true;
}

uint32_t espnow_bridge_received(void) { return s_received; }
uint32_t espnow_bridge_dropped(void)  { return s_dropped; }
