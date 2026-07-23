/**
 * @file status_led.c
 * @brief WS2812 flight-mode indicator (see status_led.h).
 */

#include "status_led.h"

#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "status_led";

#define STATUS_LED_GPIO   38
#define LED_RES_HZ        (10 * 1000 * 1000)   /* 10 MHz -> 0.1 us / RMT tick */

static rmt_channel_handle_t s_chan    = NULL;
static rmt_encoder_handle_t s_encoder = NULL;
static bool s_ready = false;

void status_led_init(void)
{
    if (s_ready) return;

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = STATUS_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz     = LED_RES_HZ,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&tx_cfg, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "RMT TX channel init failed");
        return;
    }

    /* WS2812 bit cells @ 0.1 us/tick: 0 = 0.3us H / 0.9us L, 1 = 0.9us H / 0.3us L. */
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    if (rmt_new_bytes_encoder(&enc_cfg, &s_encoder) != ESP_OK) {
        ESP_LOGE(TAG, "RMT encoder init failed");
        return;
    }

    rmt_enable(s_chan);
    s_ready = true;
    ESP_LOGI(TAG, "WS2812 status LED on GPIO%d", STATUS_LED_GPIO);
}

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ready) return;
    uint8_t grb[3] = { g, r, b };   /* WS2812 wire order is GRB */
    rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(s_chan, s_encoder, grb, sizeof(grb), &tx);
    /* WS2812 latches on the >50 us idle-low that follows each transmit. */
    rmt_tx_wait_all_done(s_chan, portMAX_DELAY);
}

static void status_led_task(void *arg)
{
    struct SysContext *ctx = (struct SysContext *)arg;
    bool on = true;

    while (1) {
        uint32_t mode = ctx->mode;
        uint8_t r = 0, g = 0, b = 0;
        bool blink = false;

        switch (mode) {
        case MODE_ARMED: g = 40;           blink = true; break;  /* green blink = armed/waiting */
        case MODE_BOOST: r = 90;                         break;  /* red   = powered flight */
        case MODE_COAST: r = 60; g = 25;                 break;  /* amber = coasting */
        case MODE_POST:  b = 45;                         break;  /* blue  = boot / self-test */
        default:         r = g = b = 12;                 break;  /* dim white = init / check */
        }

        if (blink) {
            on = !on;
            led_set(on ? r : 0, on ? g : 0, on ? b : 0);
            vTaskDelay(pdMS_TO_TICKS(400));
        } else {
            on = true;
            led_set(r, g, b);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

void status_led_start_task(struct SysContext *ctx)
{
    if (!s_ready || ctx == NULL) return;
    xTaskCreate(status_led_task, "status_led", 2560, ctx, 3, NULL);
}
