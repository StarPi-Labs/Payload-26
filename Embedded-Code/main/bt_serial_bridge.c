#include "bt_serial_bridge.h"

#include "sdkconfig.h"
#include "esp_log.h"

#if CONFIG_BT_ENABLED && CONFIG_BLUEDROID_ENABLED && CONFIG_BT_SPP_ENABLED
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"

static const char *TAG = "BT_BRIDGE";
static bool s_bt_started = false;
static bool s_bt_has_client = false;
static uint32_t s_spp_handle = 0;

static void bt_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, "SPP_SERVER");
        break;

    case ESP_SPP_SRV_OPEN_EVT:
        s_spp_handle = param->srv_open.handle;
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

    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bt controller init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
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

bool bt_serial_has_client(void)
{
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

bool bt_serial_has_client(void)
{
    return false;
}

#endif
