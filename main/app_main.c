// Sensor-Actor node: entry point. See docs/FSD.md for the requirements.
#include <stdio.h>
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "console.h"
#include "sa_config.h"
#include "sa_crash.h"
#include "sa_i2c.h"
#include "sa_log.h"
#include "actors.h"
#include "sa_mqtt.h"
#include "sensors.h"
#include "webui.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

#define OTA_CONFIRM_TIMEOUT_S 60   // REQ-OTA-2

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was full or from a newer format, erasing it");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// A freshly OTA-updated image runs in "pending verify" state. Confirm it once the node is
// reachable (WiFi or AP up and web server running); otherwise restart so the bootloader
// rolls back to the previous image.
static void confirm_ota_image(bool web_ok)
{
    esp_ota_img_states_t st;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGI(TAG, "new firmware: waiting up to %ds for WiFi before confirming it", OTA_CONFIRM_TIMEOUT_S);
    for (int i = 0; i < OTA_CONFIRM_TIMEOUT_S * 10 && !wifi_mgr_is_up(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (web_ok && wifi_mgr_is_up()) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed");
    } else {
        ESP_LOGE(TAG, "new firmware failed its self-check, rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void app_main(void)
{
    sa_log_init();
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "sensor-actor %s starting (slot %s)", app->version, esp_ota_get_running_partition()->label);

    sa_crash_init();   // logs a stored crash report into the error log
    init_nvs();
    ESP_ERROR_CHECK(sa_config_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    sa_i2c_init();   // a bus error is logged; the node keeps running without sensors
    ESP_ERROR_CHECK(wifi_mgr_start());
    bool web_ok = webui_start() == ESP_OK;
    // Actors first: they register for sensor values, so the very first sensor result
    // (e.g. "missing" at boot, which triggers a rule's fault action) is not lost.
    if (actors_init() != ESP_OK) {
        ESP_LOGE(TAG, "actor task failed to start");
    }
    if (sensors_init() != ESP_OK) {
        ESP_LOGE(TAG, "sensor task failed to start");
    }
    if (sa_mqtt_start() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT client failed to start");
    }
    console_start();
    confirm_ota_image(web_ok);
}
