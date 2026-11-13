// Sensor-Actor node: entry point. See docs/FSD.md for the requirements.
#include <stdio.h>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

static const char *TAG = "main";

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "sensor-actor %s starting (partition %s)", app->version,
             esp_ota_get_running_partition()->label);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // TODO: config, wifi_mgr, mqtt, sensors, actors, rules, web UI, OTA confirm (REQ-OTA-2)
}
