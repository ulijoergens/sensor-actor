// Pure parts of sa_config: defaults and validation (no NVS access).
#include "sa_config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void sa_config_defaults(sa_device_cfg_t *cfg, const uint8_t mac[6])
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->name, sizeof(cfg->name), "SensAct-%02X%02X", mac[4], mac[5]);
    snprintf(cfg->id, sizeof(cfg->id), "sa%02x%02x%02x", mac[3], mac[4], mac[5]);
    cfg->mqtt_port = 1883;
    cfg->discovery = true;
    strcpy(cfg->discovery_prefix, "homeassistant");
    cfg->i2c_sda = 21;
    cfg->i2c_scl = 22;
    cfg->onewire = 4;
}

bool sa_id_valid(const char *id)
{
    size_t n = strlen(id);
    if (n < 1 || n > SA_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool sa_gpio_exists(int g)
{
    // ESP32: GPIO 0-19, 21-23, 25-27, 32-39 are bonded out.
    return (g >= 0 && g <= 19) || (g >= 21 && g <= 23) || (g >= 25 && g <= 27) ||
           (g >= 32 && g <= 39);
}

bool sa_gpio_output_ok(int g)
{
    if (!sa_gpio_exists(g)) {
        return false;
    }
    if (g >= 6 && g <= 11) {
        return false;   // connected to the SPI flash
    }
    if (g >= 34) {
        return false;   // input-only
    }
    return true;
}

bool sa_gpio_is_strapping(int g)
{
    return g == 0 || g == 2 || g == 5 || g == 12 || g == 15;
}

static bool printable(const char *s)
{
    for (; *s; s++) {
        if (!isprint((unsigned char)*s)) {
            return false;
        }
    }
    return true;
}

static bool hostname_ok(const char *s)
{
    // IPv4 address or DNS name: letters, digits, '-', '.'
    if (!*s) {
        return false;
    }
    for (; *s; s++) {
        char c = *s;
        if (!(isalnum((unsigned char)c) || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

static bool prefix_ok(const char *s)
{
    if (!*s || *s == '/') {
        return false;
    }
    for (; *s; s++) {
        char c = *s;
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '/')) {
            return false;   // no MQTT wildcards or spaces
        }
    }
    return true;
}

const char *sa_config_validate(const sa_device_cfg_t *c)
{
    size_t n = strnlen(c->name, sizeof(c->name));
    if (n == 0 || n > SA_NAME_MAX || !printable(c->name)) {
        return "Device name: 1 to 32 printable characters";
    }
    if (!sa_id_valid(c->id)) {
        return "Device ID: 1 to 8 characters a-z, 0-9, _ or -";
    }
    if (strnlen(c->wifi_ssid, sizeof(c->wifi_ssid)) > SA_SSID_MAX) {
        return "WiFi SSID: at most 32 characters";
    }
    size_t p = strnlen(c->wifi_pass, sizeof(c->wifi_pass));
    if (p != 0 && (p < 8 || p > SA_PASS_MAX)) {
        return "WiFi password: empty (open network) or 8 to 64 characters";
    }
    if (c->mqtt_host[0]) {
        if (!hostname_ok(c->mqtt_host)) {
            return "MQTT broker: IP address or host name";
        }
        if (c->mqtt_port == 0) {
            return "MQTT port: 1 to 65535";
        }
    }
    if (c->discovery && !prefix_ok(c->discovery_prefix)) {
        return "Discovery prefix: letters, digits, _ - /, not starting with /";
    }
    const int8_t pins[3] = {c->i2c_sda, c->i2c_scl, c->onewire};
    const char *names[3] = {"I2C SDA pin", "I2C SCL pin", "1-Wire pin"};
    static char msg[64];
    for (int i = 0; i < 3; i++) {
        if (!sa_gpio_output_ok(pins[i])) {
            snprintf(msg, sizeof(msg), "%s: GPIO %d cannot be used", names[i], pins[i]);
            return msg;
        }
        for (int j = 0; j < i; j++) {
            if (pins[i] == pins[j]) {
                snprintf(msg, sizeof(msg), "%s: GPIO %d is already used", names[i], pins[i]);
                return msg;
            }
        }
    }
    return NULL;
}
