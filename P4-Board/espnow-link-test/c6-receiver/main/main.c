/**
 * @file main.c
 * @brief ESP-NOW link test — RECEIVER (runs on the ESP32-C6 of the
 *        ESP32-P4-Function-EV-Board).
 *
 * Partner of ../s3-sender. Listens for the broadcast test packets and reports
 * arrival rate, lost packets (derived from the sequence counter) and RSSI, so
 * the link can be characterised — not just confirmed — before anything real is
 * built on it. Walk away from the transmitter and watch RSSI fall and loss
 * climb; that curve is your usable range.
 *
 * IMPORTANT — this firmware REPLACES the factory esp-hosted slave firmware on
 * the C6. While it is loaded, the P4 has no Wi-Fi or Bluetooth at all, because
 * the co-processor is no longer speaking the esp-hosted protocol. See the
 * README for restoring the factory firmware.
 *
 * Console goes to UART0 (the PROG_C6 header), set in sdkconfig.defaults.
 */

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"

static const char *TAG = "espnow_rx";

/* Must match the transmitter. A mismatch here is the most common cause of
 * "everything looks right but nothing arrives". */
#define ESPNOW_CHANNEL   6

#define REPORT_PERIOD_MS 2000

/* Keep byte-identical to the copy in ../s3-sender/main/main.c. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint32_t uptime_ms;
} espnow_test_pkt_t;

#define PILG_MAGIC  0x474C4950u   /* "PILG" little-endian */

/* Written from the ESP-NOW receive callback, read by the reporting loop.
 * The callback runs in Wi-Fi task context, so it does the minimum possible:
 * no logging, no blocking, just counters. */
static volatile uint32_t s_received;
static volatile uint32_t s_lost;
static volatile uint32_t s_foreign;     /* packets that weren't ours */
static volatile int32_t  s_last_rssi;
static volatile uint32_t s_last_seq;
static volatile bool     s_have_seq;
static volatile int64_t  s_last_rx_us;
static uint8_t           s_peer_mac[ESP_NOW_ETH_ALEN];

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    if (len != (int)sizeof(espnow_test_pkt_t)) {
        s_foreign++;
        return;
    }

    espnow_test_pkt_t pkt;
    memcpy(&pkt, data, sizeof(pkt));
    if (pkt.magic != PILG_MAGIC) {
        s_foreign++;      /* someone else's ESP-NOW traffic — ignore quietly */
        return;
    }

    if (s_have_seq && pkt.seq > s_last_seq + 1) {
        s_lost += pkt.seq - s_last_seq - 1;
    }
    s_last_seq  = pkt.seq;
    s_have_seq  = true;
    s_received++;
    s_last_rx_us = esp_timer_get_time();

    if (info->rx_ctrl) {
        s_last_rssi = info->rx_ctrl->rssi;
    }
    if (info->src_addr) {
        memcpy(s_peer_mac, info->src_addr, ESP_NOW_ETH_ALEN);
    }
}

static void wifi_init_for_espnow(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    wifi_init_for_espnow();
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "ESP-NOW receiver up (ESP32-C6)");
    ESP_LOGI(TAG, "  my MAC   : %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "  channel  : %d", ESPNOW_CHANNEL);
    ESP_LOGI(TAG, "Waiting for packets from the S3 transmitter...");

    uint32_t prev_received = 0;
    bool     announced     = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));

        uint32_t received = s_received;
        uint32_t lost     = s_lost;
        uint32_t foreign  = s_foreign;

        if (received == 0) {
            ESP_LOGW(TAG, "nothing received yet — check that both ends are on "
                          "channel %d and the transmitter is powered",
                     ESPNOW_CHANNEL);
            continue;
        }

        if (!announced) {
            ESP_LOGI(TAG, "LINK UP — first packet from "
                          "%02X:%02X:%02X:%02X:%02X:%02X",
                     s_peer_mac[0], s_peer_mac[1], s_peer_mac[2],
                     s_peer_mac[3], s_peer_mac[4], s_peer_mac[5]);
            announced = true;
        }

        uint32_t in_window = received - prev_received;
        prev_received = received;

        int64_t silent_ms = (esp_timer_get_time() - s_last_rx_us) / 1000;
        if (in_window == 0) {
            ESP_LOGW(TAG, "no packets for %" PRId64 " ms — out of range?", silent_ms);
            continue;
        }

        /* Loss percentage is over the whole session, so it settles into a
         * meaningful figure rather than jumping around window to window. */
        uint32_t expected = received + lost;
        float loss_pct = expected ? (100.0f * (float)lost / (float)expected) : 0.0f;

        ESP_LOGI(TAG, "rx %" PRIu32 " (+%" PRIu32 ")  lost %" PRIu32
                      " (%.1f%%)  rssi %" PRId32 " dBm%s",
                 received, in_window, lost, loss_pct, s_last_rssi,
                 foreign ? "  [ignoring other ESP-NOW traffic]" : "");
    }
}
