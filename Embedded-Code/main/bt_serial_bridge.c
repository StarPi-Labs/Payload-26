#include "bt_serial_bridge.h"

#include "sdkconfig.h"
#include "esp_log.h"

#if CONFIG_BT_ENABLED && CONFIG_BLUEDROID_ENABLED && CONFIG_BT_SPP_ENABLED
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"

static const char *TAG = "BT_BRIDGE";
static bool s_bt_started = false;
static bool s_bt_has_client = false;
static uint32_t s_spp_handle = 0;

static esp_bt_mode_t bt_controller_mode_for_build(void)
{
#if CONFIG_BTDM_CTRL_MODE_BTDM
    return ESP_BT_MODE_BTDM;
#elif CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY
    return ESP_BT_MODE_CLASSIC_BT;
#elif CONFIG_BTDM_CTRL_MODE_BLE_ONLY
    return ESP_BT_MODE_BLE;
#else
    return ESP_BT_MODE_CLASSIC_BT;
#endif
}

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (event == ESP_BT_GAP_MODE_CHG_EVT) {
        ESP_LOGI(TAG, "GAP mode changed: mode=%d", param->mode_chg.mode);
    }
}

static void bt_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, "SPP_SERVER");
        break;

    case ESP_SPP_SRV_OPEN_EVT:
        s_spp_handle = param->srv_open.handle;
        ESP_LOGI(TAG, "Real Handle %ld", s_spp_handle);
        s_bt_has_client = true;
        break;

    case ESP_SPP_OPEN_EVT:
        s_spp_handle = param->open.handle;
        s_bt_has_client = true;
        break;

    case ESP_SPP_CLOSE_EVT:
        s_bt_has_client = false;
        s_spp_handle = 0;
        break;

    default:
        break;
    }
}

bool bt_serial_init(const char *device_name)
{
    if (s_bt_started) {
        return true;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_bt_mode_t bt_mode = bt_controller_mode_for_build();

#if CONFIG_BTDM_CTRL_MODE_BLE_ONLY
    ESP_LOGE(TAG, "Controller is configured BLE-only. Classic SPP requires BTDM or BR/EDR-only mode.");
    return false;
#endif

    if (bt_mode == ESP_BT_MODE_CLASSIC_BT) {
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    // bt_cfg.mode = bt_mode;
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bt controller init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bt_controller_enable(bt_mode);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bt controller enable failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bt_gap_register_callback(bt_gap_cb);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gap callback register failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gap set scan mode failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_spp_register_callback(bt_spp_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spp callback register failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_spp_init(ESP_SPP_MODE_CB);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spp init failed: %s", esp_err_to_name(ret));
        return false;
    }

    if (device_name != NULL) {
        esp_bt_dev_set_device_name(device_name);
    } else {
        esp_bt_dev_set_device_name("GPS-Serial-Bluetooth");
    }

    s_bt_started = true;
    // esp_bt_sleep_disable();
    ESP_LOGI(TAG, "Classic BT SPP initialized");
    return true;
}

void bt_serial_write_byte(uint8_t byte)
{
    if (!s_bt_started || !s_bt_has_client || s_spp_handle == 0) {
        return;
    }

    esp_spp_write(s_spp_handle, 1, &byte);
}

void bt_serial_write_chunk(uint8_t *data, uint16_t len) {
    if (!s_bt_started || !s_bt_has_client || s_spp_handle == 0 || len == 0) {
        return;
    }
    
    esp_err_t err = esp_spp_write(s_spp_handle, len, data);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "BT Write Failed! Err: %s\n", esp_err_to_name(err) ); 
    }
}


bool bt_serial_has_client(void) {
    return s_bt_started && s_bt_has_client;
}

#else

static const char *TAG = "BT_BRIDGE";

bool bt_serial_init(const char *device_name)
{
    (void)device_name;
    ESP_LOGW(TAG, "Bluetooth SPP is disabled in sdkconfig");
    return false;
}

void bt_serial_write_byte(uint8_t byte)
{
    (void)byte;
}

void bt_serial_write_chunk(uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
}

bool bt_serial_has_client(void)
{
    return false;
}

#endif
