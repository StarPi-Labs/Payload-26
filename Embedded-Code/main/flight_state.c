/**
 * @file flight_state.c
 * @brief Autonomous flight state machine (see flight_state.h).
 */

#include "flight_state.h"

#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "flight";

#define NVS_NS      "flight"
#define NVS_KEY     "mode"
#define BOOT_GPIO   GPIO_NUM_0

/* Rolling window over |accel| [g] */
static float   s_win[FLIGHT_WINDOW_SAMPLES];
static int     s_idx = 0;
static int     s_cnt = 0;
static int64_t s_boost_us = 0;   /* time BOOST was entered (us) */

static const char *mode_name(uint32_t m)
{
    switch (m) {
    case MODE_ARMED: return "ARMED";
    case MODE_BOOST: return "BOOST";
    case MODE_COAST: return "COAST";
    default:         return "?";
    }
}

static void flight_save(uint32_t mode)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY, (uint8_t)mode);
        nvs_commit(h);
        nvs_close(h);
    }
}

static uint32_t flight_load(void)
{
    nvs_handle_t h;
    uint8_t m = MODE_ARMED;   /* default if never saved */
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY, &m);   /* leaves m unchanged if the key is missing */
        nvs_close(h);
    }
    return m;
}

static void flight_set_mode(struct SysContext *ctx, uint32_t mode)
{
    ctx->mode = mode;
    flight_save(mode);
    if (mode == MODE_BOOST) s_boost_us = esp_timer_get_time();
    ESP_LOGW(TAG, "state -> %s", mode_name(mode));
}

static uint32_t s_boot_mode    = MODE_ARMED;
static bool     s_preinit_done = false;

void flight_state_preinit(void)
{
    if (s_preinit_done) return;

    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    gpio_set_direction(BOOT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_GPIO, GPIO_PULLUP_ONLY);

    /* Re-arm: press BOOT within FLIGHT_REARM_WAIT_MS of startup and hold it for
     * FLIGHT_REARM_HOLD_MS. The wait window exists because the button must be
     * pressed *after* reset — holding it through reset straps the chip into ROM
     * download mode instead of running the app. */
    ESP_LOGW(TAG, "hold BOOT now (~%d ms) to re-arm...", FLIGHT_REARM_HOLD_MS);
    bool rearm = false;
    for (int w = 0; w < FLIGHT_REARM_WAIT_MS; w += 50) {
        if (gpio_get_level(BOOT_GPIO) == 0) {
            rearm = true;
            for (int t = 0; t < FLIGHT_REARM_HOLD_MS; t += 50) {
                vTaskDelay(pdMS_TO_TICKS(50));
                if (gpio_get_level(BOOT_GPIO) != 0) { rearm = false; break; }
            }
            break;   /* one press attempt decides it */
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (rearm) {
        s_boot_mode = MODE_ARMED;
        flight_save(MODE_ARMED);
        ESP_LOGW(TAG, "BOOT held -> re-armed to ARMED");
    } else {
        s_boot_mode = flight_load();
        ESP_LOGW(TAG, "resumed mode=%s from NVS", mode_name(s_boot_mode));
    }
    s_preinit_done = true;
}

uint32_t flight_state_boot_mode(void)
{
    return s_boot_mode;
}

void flight_state_init(struct SysContext *ctx)
{
    flight_state_preinit();   /* no-op if already resolved */
    ctx->mode = s_boot_mode;
    if (s_boot_mode == MODE_BOOST) s_boost_us = esp_timer_get_time();
}

void flight_state_update(struct SysContext *ctx, float accel_g)
{
    s_win[s_idx] = accel_g;
    s_idx = (s_idx + 1) % FLIGHT_WINDOW_SAMPLES;
    if (s_cnt < FLIGHT_WINDOW_SAMPLES) s_cnt++;

    float sum = 0.0f;
    for (int i = 0; i < s_cnt; i++) sum += s_win[i];
    float avg = sum / (float)s_cnt;

    switch (ctx->mode) {
    case MODE_ARMED:
        if (avg >= FLIGHT_LAUNCH_TRIP_G) {
            flight_set_mode(ctx, MODE_BOOST);
        }
        break;
    case MODE_BOOST:
        if (avg < FLIGHT_LAUNCH_G &&
            (esp_timer_get_time() - s_boost_us) >= (int64_t)FLIGHT_MIN_BOOST_MS * 1000) {
            flight_set_mode(ctx, MODE_COAST);
        }
        break;
    default:   /* COAST terminal; INIT/POST/etc. no auto-transition */
        break;
    }
}
