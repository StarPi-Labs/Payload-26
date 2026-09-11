/**
 * @file main.c
 * @brief ESP-NOW link test — TRANSMITTER (runs on the rocket-side ESP32-S3).
 *
 * Broadcasts a small sequenced packet twice a second. Its partner is
 * ../c6-receiver, which runs on the ESP32-C6 co-processor of the
 * ESP32-P4-Function-EV-Board and reports packet loss and RSSI.
 *
 * This is a link test only — no sensors, no framing, no SD. The point is to
 * answer one question before any real work is built on top: does ESP-NOW
 * actually carry packets between these two boards, and how far?
 *
 * Why the C6 and not the P4: the ESP32-P4 has no radio of its own. It borrows
 * the on-board C6 over SDIO through esp-hosted, and esp-hosted does not proxy
 * the ESP-NOW API to the host (espressif/esp-hosted-mcu issue #19, open and
 * unanswered since Nov 2024). The C6 itself is an ordinary ESP32-C6 where
 * ESP-NOW works natively, so the radio work belongs there.
 */

#include <string.h>

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

static const char *TAG = "espnow_tx";

/* Both ends MUST agree on this. A channel mismatch is the single most common
 * reason an otherwise-correct ESP-NOW setup receives nothing at all. */
#define ESPNOW_CHANNEL   6

#define SEND_PERIOD_MS   500

static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

/* Keep byte-identical to the copy in ../c6-receiver/main/main.c. */
typedef struct __attribute__((packed)) {
    uint32_t magic;       /* PILG_MAGIC — lets the receiver ignore other traffic */
    uint32_t seq;         /* increments every packet; receiver derives loss     */
    uint32_t uptime_ms;
} espnow_test_pkt_t;

#define PILG_MAGIC  0x474C4950u   /* "PILG" little-endian */

static void wifi_init_for_espnow(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    /* Nothing to persist for a link test, and RAM-only storage avoids
     * surprises from whatever was last written to NVS by other firmware. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Must come after esp_wifi_start(), and the peer below uses channel 0
     * ("whatever channel we are on") so the two can never disagree. */
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    /* Power save would let the radio doze between beacons and adds latency
     * for no benefit here. */
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

    esp_now_peer_info_t peer = {
        .channel = 0,        /* 0 = use the interface's current channel */
        .ifidx   = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, BROADCAST_MAC, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "ESP-NOW transmitter up");
    ESP_LOGI(TAG, "  my MAC   : %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "  channel  : %d", ESPNOW_CHANNEL);
    ESP_LOGI(TAG, "  period   : %d ms (broadcast)", SEND_PERIOD_MS);
    ESP_LOGI(TAG, "Watch the C6 receiver console for arrivals.");

    espnow_test_pkt_t pkt = { .magic = PILG_MAGIC, .seq = 0 };
    uint32_t send_failures = 0;

    while (1) {
        pkt.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);

        /* esp_now_send() returning ESP_OK only means the packet was queued to
         * the radio. ESP-NOW broadcast is unacknowledged, so genuine proof of
         * delivery can only come from the receiver's console — deliberately
         * not using a send callback here, since its signature changed across
         * IDF versions and would risk a build break in a throwaway test. */
        esp_err_t res = esp_now_send(BROADCAST_MAC, (uint8_t *)&pkt, sizeof(pkt));
        if (res != ESP_OK) {
            if ((send_failures++ % 20) == 0) {
                ESP_LOGW(TAG, "esp_now_send failed: %s (%lu total)",
                         esp_err_to_name(res), (unsigned long)send_failures);
            }
        } else if ((pkt.seq % 20) == 0) {
            ESP_LOGI(TAG, "sent seq=%lu", (unsigned long)pkt.seq);
        }

        pkt.seq++;
        vTaskDelay(pdMS_TO_TICKS(SEND_PERIOD_MS));
    }
}
