// Device settings (docs/FSD.md §7 Settings page, REQ-HW-1/2, REQ-PER-1).
//
// Settings live in NVS (namespace "sa") as one versioned blob. Sensors, actors and rules
// get their own records later. sa_config_validate() and the GPIO helpers are pure
// functions so they can be unit-tested.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SA_NAME_MAX     32   // label / device name length (FSD §3)
#define SA_ID_MAX       8    // short ID length (FSD §3)
#define SA_SSID_MAX     32
#define SA_PASS_MAX     64
#define SA_HOST_MAX     64
#define SA_USER_MAX     32
#define SA_PREFIX_MAX   32

typedef struct {
    char name[SA_NAME_MAX + 1];          // device name, also used as hostname
    char id[SA_ID_MAX + 1];              // device ID, used in MQTT topics
    char wifi_ssid[SA_SSID_MAX + 1];     // empty: start in AP mode (REQ-WIFI-3)
    char wifi_pass[SA_PASS_MAX + 1];     // empty (open network) or 8..64 chars
    char mqtt_host[SA_HOST_MAX + 1];     // empty: MQTT disabled
    uint16_t mqtt_port;
    char mqtt_user[SA_USER_MAX + 1];
    char mqtt_pass[SA_PASS_MAX + 1];
    bool discovery;                      // REQ-MQ-4
    char discovery_prefix[SA_PREFIX_MAX + 1];
    int8_t i2c_sda;
    int8_t i2c_scl;
    int8_t onewire;
} sa_device_cfg_t;

// Factory defaults. The MAC gives a unique device ID ("sa" + last 3 bytes, hex) and
// name ("SensAct-XXXX", last 2 bytes).
void sa_config_defaults(sa_device_cfg_t *cfg, const uint8_t mac[6]);

// NULL if valid, otherwise a short English error message naming the field.
const char *sa_config_validate(const sa_device_cfg_t *cfg);

bool sa_id_valid(const char *id);              // 1..8 chars of [a-z0-9_-]
bool sa_gpio_exists(int gpio);                 // GPIO number exists on the ESP32
bool sa_gpio_output_ok(int gpio);              // can drive an output (not flash/input-only)
bool sa_gpio_is_strapping(int gpio);           // 0, 2, 5, 12, 15: usable with care

// Loads settings from NVS, or factory defaults if there are none (or the stored record
// is from an incompatible version). Call after nvs_flash_init().
esp_err_t sa_config_init(void);

// Current settings (valid after sa_config_init()).
const sa_device_cfg_t *sa_config_get(void);

// Validates and stores new settings. Returns ESP_ERR_INVALID_ARG if invalid.
esp_err_t sa_config_save(const sa_device_cfg_t *cfg);

// Erases all stored settings (REQ-PER-1). Takes effect after a reboot.
esp_err_t sa_config_factory_reset(void);

#ifdef __cplusplus
}
#endif
