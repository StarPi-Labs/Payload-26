/**
 * @file app_main.c
 * @brief Pi-LOG ground station - ESP32-C6 co-processor.
 *
 * esp-hosted runs the co-processor side of the P4 link by itself (transport,
 * RPC, peer data are all brought up by the component). The only thing this
 * application adds is the ESP-NOW bridge, which forwards the rocket's
 * telemetry to the P4 over the peer-data channel.
 *
 * Derived from Espressif's esp_hosted 3.0.9 example peer_data_transfer/cp
 * (Apache-2.0); the example's echo demo is replaced by the bridge.
 */

#include "esp_event.h"
#include "esp_log.h"

#include "espnow_bridge.h"

static const char *TAG = "pilog_cp";

void app_main(void)
{
    /* Wi-Fi posts its events here. ESP_ERR_INVALID_STATE just means
     * esp-hosted created it first. */
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Pi-LOG C6 co-processor: esp-hosted + ESP-NOW bridge");
    espnow_bridge_init();
}
