/**
 * @file telemetry_net.c
 * @brief Wi-Fi SoftAP + UDP broadcast transport — implementation.
 *
 * Why SoftAP + UDP and not ESP-NOW: the ground display is an ESP32-P4, which
 * has no radio of its own. It borrows the on-board ESP32-C6 through esp-hosted,
 * and that slave firmware implements station / SoftAP / SoftAP+station only —
 * ESP-NOW is not among its supported features. Plain Wi-Fi sockets are the
 * path that works on both ends without reflashing anything.
 *
 * The payload runs the AP rather than joining one, so the link is
 * self-sufficient: power the rocket up and the network exists, whether or not
 * the display (or a laptop) happens to be on.
 *
 * Flow control: telemetry_net_send() copies into a ring and returns; a
 * dedicated low-priority task drains it into datagrams. When the ring is full
 * the newest chunk is dropped whole — never split — so the receiver loses one
 * frame and resyncs on the next sync word instead of decoding a spliced one.
 */

#include "telemetry_net.h"
#include "sdkconfig.h"

#if CONFIG_ENABLE_TELEMETRY_NET

#include <string.h>
#include <errno.h>
#include <unistd.h>          /* close() on the socket fd */

#include <sys/socket.h>
#include <netinet/in.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "telem_net";

#define RING_CAP    (16 * 1024)  /* backlog tolerated before dropping        */
#define DGRAM_MAX   1400         /* stays under a 1500 B MTU with UDP/IP hdrs */
#define TX_IDLE_MS  20           /* poll interval when the ring is empty      */
#define LOCK_WAIT_MS 2           /* producers never wait longer than this     */

static uint8_t           s_ring[RING_CAP];
static size_t            s_head;    /* write cursor */
static size_t            s_tail;    /* read cursor  */
static size_t            s_used;
static SemaphoreHandle_t s_lock;
static int               s_sock = -1;
static volatile bool     s_started;
static volatile uint8_t  s_clients;
static volatile uint32_t s_dropped;

/* ── Ring buffer (caller holds s_lock) ────────────────────────────────── */

/* Copies nothing and returns false unless the whole chunk fits, so a frame is
 * never half-written into the stream. */
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

/* ── Wi-Fi events ─────────────────────────────────────────────────────── */

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != WIFI_EVENT) return;

    if (id == WIFI_EVENT_AP_STACONNECTED) {
        if (s_clients < UINT8_MAX) s_clients++;
        ESP_LOGI(TAG, "client joined (%u connected)", (unsigned)s_clients);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_clients) s_clients--;
        ESP_LOGI(TAG, "client left (%u connected)", (unsigned)s_clients);
    }
}

/* ── Drain task ───────────────────────────────────────────────────────── */

static void telemetry_net_task(void *arg)
{
    (void)arg;
    static uint8_t dgram[DGRAM_MAX];

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(CONFIG_TELEMETRY_NET_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    while (1) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t n = ring_get(dgram, sizeof(dgram));
        xSemaphoreGive(s_lock);

        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(TX_IDLE_MS));
            continue;
        }

        /* Nobody listening: keep draining so the ring can't wedge, but don't
         * bother putting anything on the air. */
        if (s_clients == 0 || s_sock < 0) continue;

        if (sendto(s_sock, dgram, n, 0, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
            ESP_LOGW(TAG, "sendto failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

bool telemetry_net_init(void)
{
    if (s_started) return true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s — link disabled", esp_err_to_name(err));
        return false;
    }

    /* These may already exist if something else brought the stack up first. */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init failed: %s — link disabled", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop failed: %s — link disabled", esp_err_to_name(err));
        return false;
    }

    if (esp_netif_create_default_wifi_ap() == NULL) {
        ESP_LOGE(TAG, "AP netif creation failed — link disabled");
        return false;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s — link disabled", esp_err_to_name(err));
        return false;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_cb, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "event handler register failed: %s — client count will read 0",
                 esp_err_to_name(err));
    }

    const char *ssid = CONFIG_TELEMETRY_NET_SSID;
    const char *pass = CONFIG_TELEMETRY_NET_PASSWORD;

    wifi_config_t ap_cfg = { 0 };
    size_t ssid_len = strlen(ssid);
    if (ssid_len > sizeof(ap_cfg.ap.ssid)) ssid_len = sizeof(ap_cfg.ap.ssid);
    memcpy(ap_cfg.ap.ssid, ssid, ssid_len);
    ap_cfg.ap.ssid_len       = (uint8_t)ssid_len;
    ap_cfg.ap.channel        = CONFIG_TELEMETRY_NET_CHANNEL;
    ap_cfg.ap.max_connection = 4;
    /* WPA2 needs 8 characters; anything shorter (or empty) means open, which is
     * legitimate for a field test where typing a password is a nuisance. */
    if (strlen(pass) >= 8) {
        strncpy((char *)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP start failed — link disabled");
        return false;
    }

    /* Predictable latency matters more than the few mA power save would buy on
     * a payload that is already running sensors flat out. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d — link disabled", errno);
        return false;
    }
    int on = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "mutex alloc failed — link disabled");
        close(s_sock);
        s_sock = -1;
        return false;
    }

    /* Low priority on purpose: sensor sampling and SD writes must always win. */
    if (xTaskCreate(telemetry_net_task, "telem_net", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed — link disabled");
        close(s_sock);
        s_sock = -1;
        return false;
    }

    s_started = true;
    ESP_LOGI(TAG, "SoftAP \"%s\" (ch %d, %s) — broadcasting UDP :%d",
             ssid, CONFIG_TELEMETRY_NET_CHANNEL,
             (ap_cfg.ap.authmode == WIFI_AUTH_OPEN) ? "open" : "WPA2",
             CONFIG_TELEMETRY_NET_PORT);
    return true;
}

void telemetry_net_send(const uint8_t *data, size_t len)
{
    if (!s_started || data == NULL || len == 0) return;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOCK_WAIT_MS)) != pdTRUE) {
        s_dropped++;   /* logger keeps moving; this chunk simply doesn't fly */
        return;
    }
    bool queued = ring_put(data, len);
    xSemaphoreGive(s_lock);

    if (!queued && (++s_dropped % 100) == 1) {
        ESP_LOGW(TAG, "link saturated — %lu chunks dropped so far",
                 (unsigned long)s_dropped);
    }
}

bool telemetry_net_has_client(void)
{
    return s_started && s_clients > 0;
}

uint32_t telemetry_net_dropped(void)
{
    return s_dropped;
}

#else /* !CONFIG_ENABLE_TELEMETRY_NET — compiled out, callers need no #ifdef */

bool     telemetry_net_init(void)                             { return false; }
void     telemetry_net_send(const uint8_t *d, size_t l)       { (void)d; (void)l; }
bool     telemetry_net_has_client(void)                       { return false; }
uint32_t telemetry_net_dropped(void)                          { return 0; }

#endif /* CONFIG_ENABLE_TELEMETRY_NET */
