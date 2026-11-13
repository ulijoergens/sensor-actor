// sa_config: NVS storage of the device settings.
#include "sa_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

#define NVS_NS      "sa"
#define KEY_DEVICE  "dev"
#define DEV_VERSION 1

typedef struct {
    uint16_t version;
    uint16_t size;
    sa_device_cfg_t cfg;
} stored_device_t;

static sa_device_cfg_t s_cfg;

static void load_defaults(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    sa_config_defaults(&s_cfg, mac);
}

esp_err_t sa_config_init(void)
{
    load_defaults();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored settings, using defaults (%s / %s)", s_cfg.name, s_cfg.id);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    stored_device_t rec;
    size_t len = sizeof(rec);
    err = nvs_get_blob(h, KEY_DEVICE, &rec, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored settings, using defaults (%s / %s)", s_cfg.name, s_cfg.id);
        return ESP_OK;
    }
    if (err != ESP_OK || len != sizeof(rec) || rec.version != DEV_VERSION ||
        rec.size != sizeof(sa_device_cfg_t)) {
        ESP_LOGW(TAG, "stored settings unreadable or from another version, using defaults");
        return ESP_OK;
    }
    const char *why = sa_config_validate(&rec.cfg);
    if (why) {
        ESP_LOGW(TAG, "stored settings invalid (%s), using defaults", why);
        return ESP_OK;
    }
    s_cfg = rec.cfg;
    ESP_LOGI(TAG, "settings loaded: %s / %s", s_cfg.name, s_cfg.id);
    return ESP_OK;
}

const sa_device_cfg_t *sa_config_get(void)
{
    return &s_cfg;
}

esp_err_t sa_config_save(const sa_device_cfg_t *cfg)
{
    const char *why = sa_config_validate(cfg);
    if (why) {
        ESP_LOGW(TAG, "rejected settings: %s", why);
        return ESP_ERR_INVALID_ARG;
    }
    stored_device_t rec = {.version = DEV_VERSION, .size = sizeof(sa_device_cfg_t), .cfg = *cfg};

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY_DEVICE, &rec, sizeof(rec));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_cfg = *cfg;
        ESP_LOGI(TAG, "settings saved");
    } else {
        ESP_LOGE(TAG, "saving settings failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t sa_config_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGW(TAG, "factory reset: all settings erased");
    return err;
}
