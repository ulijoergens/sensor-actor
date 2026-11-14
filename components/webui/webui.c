// Web UI server, see webui.h.
#include "webui.h"
#include "webui_priv.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sa_config.h"
#include "sa_log.h"
#include "sa_mqtt.h"
#include "wifi_mgr.h"

static const char *TAG = "web";

#define MAX_BODY 1024

// ---------------------------------------------------------------- static files

#define ASSET(name)                                                        \
    extern const char name##_start[] asm("_binary_" #name "_start");       \
    extern const char name##_end[] asm("_binary_" #name "_end")

ASSET(index_html);
ASSET(app_js);
ASSET(style_css);

typedef struct {
    const char *start;
    const char *end;
    const char *type;
} asset_t;

static esp_err_t send_asset(httpd_req_t *req)
{
    const asset_t *a = req->user_ctx;
    httpd_resp_set_type(req, a->type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    // EMBED_TXTFILES appends a NUL terminator; don't send it.
    return httpd_resp_send(req, a->start, a->end - a->start - 1);
}

static const asset_t s_index = {index_html_start, index_html_end, "text/html; charset=utf-8"};
static const asset_t s_app = {app_js_start, app_js_end, "application/javascript"};
static const asset_t s_css = {style_css_start, style_css_end, "text/css"};

// ---------------------------------------------------------------- helpers

esp_err_t webui_send_json(httpd_req_t *req, cJSON *root)
{
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt);
    free(txt);
    return err;
}

esp_err_t webui_send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    httpd_resp_set_status(req, status);
    return webui_send_json(req, o);
}

static void reboot_cb(void *arg)
{
    esp_restart();
}

static void reboot_soon(void)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = {.callback = reboot_cb, .name = "reboot"};
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 1500 * 1000);   // let the HTTP response go out first
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT: return "reset pin";
    default: return "other";
    }
}

// ---------------------------------------------------------------- API handlers

static esp_err_t api_status(httpd_req_t *req)
{
    const sa_device_cfg_t *c = sa_config_get();
    const esp_app_desc_t *app = esp_app_get_description();
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);

    cJSON *root = cJSON_CreateObject();
    cJSON *d = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(d, "name", c->name);
    cJSON_AddStringToObject(d, "id", c->id);
    cJSON_AddStringToObject(d, "firmware", app->version);
    cJSON_AddStringToObject(d, "idf", app->idf_ver);
    cJSON_AddStringToObject(d, "partition", esp_ota_get_running_partition()->label);
    cJSON_AddNumberToObject(d, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(d, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(d, "heap_min", esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(d, "reset_reason", reset_reason());

    cJSON *wj = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wj, "state", wifi_mgr_state_name(w.state));
    cJSON_AddStringToObject(wj, "ssid", w.ssid);
    cJSON_AddStringToObject(wj, "ip", w.ip);
    if (w.state == WIFI_MGR_CONNECTED) {
        cJSON_AddNumberToObject(wj, "rssi", w.rssi);
    }
    cJSON_AddBoolToObject(wj, "ap_active", w.ap_active);
    cJSON_AddStringToObject(wj, "ap_ssid", w.ap_ssid);
    cJSON_AddStringToObject(wj, "ap_ip", w.ap_ip);
    cJSON_AddNumberToObject(wj, "lost_for_s", w.lost_for_s);
    cJSON_AddNumberToObject(wj, "next_retry_s", w.next_retry_s);
    cJSON_AddNumberToObject(wj, "attempts", w.attempts);

    sa_mqtt_status_t ms;
    sa_mqtt_get_status(&ms);
    cJSON *m = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddStringToObject(m, "state", !ms.configured ? "not configured"
                                        : ms.connected ? "connected" : "disconnected");
    cJSON_AddStringToObject(m, "broker", ms.broker);
    cJSON_AddNumberToObject(m, "connects", ms.connects);
    cJSON_AddNumberToObject(m, "published", ms.published);
    cJSON_AddNumberToObject(m, "disconnected_s", ms.disconnected_s);
    cJSON_AddStringToObject(m, "last_error", ms.last_error);

    cJSON *l = cJSON_AddObjectToObject(root, "log");
    cJSON_AddNumberToObject(l, "count", sa_log_count());
    return webui_send_json(req, root);
}

static esp_err_t api_settings_get(httpd_req_t *req)
{
    const sa_device_cfg_t *c = sa_config_get();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", c->name);
    cJSON_AddStringToObject(o, "id", c->id);
    cJSON_AddStringToObject(o, "wifi_ssid", c->wifi_ssid);
    cJSON_AddBoolToObject(o, "wifi_pass_set", c->wifi_pass[0] != '\0');
    cJSON_AddStringToObject(o, "mqtt_host", c->mqtt_host);
    cJSON_AddNumberToObject(o, "mqtt_port", c->mqtt_port);
    cJSON_AddStringToObject(o, "mqtt_user", c->mqtt_user);
    cJSON_AddBoolToObject(o, "mqtt_pass_set", c->mqtt_pass[0] != '\0');
    cJSON_AddBoolToObject(o, "discovery", c->discovery);
    cJSON_AddStringToObject(o, "discovery_prefix", c->discovery_prefix);
    cJSON_AddNumberToObject(o, "i2c_sda", c->i2c_sda);
    cJSON_AddNumberToObject(o, "i2c_scl", c->i2c_scl);
    cJSON_AddNumberToObject(o, "onewire", c->onewire);
    return webui_send_json(req, o);
}

// Copies a string field if present. Returns an error message or NULL.
static const char *take_str(cJSON *o, const char *key, char *dst, size_t len, const char *label)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v) {
        return NULL;
    }
    if (!cJSON_IsString(v)) {
        return label;
    }
    if (strlen(v->valuestring) >= len) {
        return label;
    }
    strcpy(dst, v->valuestring);
    return NULL;
}

static const char *take_int(cJSON *o, const char *key, int min, int max, int *dst, const char *label)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v) {
        return NULL;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max ||
        v->valuedouble != (int)v->valuedouble) {
        return label;
    }
    *dst = (int)v->valuedouble;
    return NULL;
}

cJSON *webui_read_json(httpd_req_t *req, const char **why)
{
    *why = NULL;
    if (req->content_len <= 0 || req->content_len > MAX_BODY) {
        *why = "Request body missing or too large";
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        *why = "Out of memory";
        return NULL;
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            *why = "Request body incomplete";
            return NULL;
        }
        got += r;
    }
    body[got] = '\0';
    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(in)) {
        cJSON_Delete(in);
        *why = "Invalid JSON";
        return NULL;
    }
    return in;
}

static esp_err_t api_settings_post(httpd_req_t *req)
{
    const char *why;
    cJSON *in = webui_read_json(req, &why);
    if (!in) {
        return webui_send_error(req, "400 Bad Request", why);
    }

    sa_device_cfg_t c = *sa_config_get();
    const char *bad = NULL;
    int port = c.mqtt_port, sda = c.i2c_sda, scl = c.i2c_scl, ow = c.onewire;
    char wifi_pass[SA_PASS_MAX + 1] = "", mqtt_pass[SA_PASS_MAX + 1] = "";

    if (!bad) bad = take_str(in, "name", c.name, sizeof(c.name), "Device name: 1 to 32 printable characters");
    if (!bad) bad = take_str(in, "id", c.id, sizeof(c.id), "Device ID: 1 to 8 characters a-z, 0-9, _ or -");
    if (!bad) bad = take_str(in, "wifi_ssid", c.wifi_ssid, sizeof(c.wifi_ssid), "WiFi SSID: at most 32 characters");
    if (!bad) bad = take_str(in, "wifi_pass", wifi_pass, sizeof(wifi_pass), "WiFi password: at most 64 characters");
    if (!bad) bad = take_str(in, "mqtt_host", c.mqtt_host, sizeof(c.mqtt_host), "MQTT broker: at most 64 characters");
    if (!bad) bad = take_str(in, "mqtt_user", c.mqtt_user, sizeof(c.mqtt_user), "MQTT user: at most 32 characters");
    if (!bad) bad = take_str(in, "mqtt_pass", mqtt_pass, sizeof(mqtt_pass), "MQTT password: at most 64 characters");
    if (!bad) bad = take_str(in, "discovery_prefix", c.discovery_prefix, sizeof(c.discovery_prefix), "Discovery prefix: at most 32 characters");
    if (!bad) bad = take_int(in, "mqtt_port", 1, 65535, &port, "MQTT port: 1 to 65535");
    if (!bad) bad = take_int(in, "i2c_sda", 0, 39, &sda, "I2C SDA pin: GPIO number");
    if (!bad) bad = take_int(in, "i2c_scl", 0, 39, &scl, "I2C SCL pin: GPIO number");
    if (!bad) bad = take_int(in, "onewire", 0, 39, &ow, "1-Wire pin: GPIO number");
    cJSON *disc = cJSON_GetObjectItemCaseSensitive(in, "discovery");
    if (!bad && disc) {
        if (!cJSON_IsBool(disc)) {
            bad = "Discovery: true or false";
        } else {
            c.discovery = cJSON_IsTrue(disc);
        }
    }
    // Passwords are write-only: a non-empty value replaces the stored one; "*_clear" empties it.
    if (wifi_pass[0]) {
        strcpy(c.wifi_pass, wifi_pass);
    } else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(in, "wifi_pass_clear"))) {
        c.wifi_pass[0] = '\0';
    }
    if (mqtt_pass[0]) {
        strcpy(c.mqtt_pass, mqtt_pass);
    } else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(in, "mqtt_pass_clear"))) {
        c.mqtt_pass[0] = '\0';
    }
    cJSON_Delete(in);
    if (bad) {
        return webui_send_error(req, "400 Bad Request", bad);
    }
    c.mqtt_port = (uint16_t)port;
    c.i2c_sda = (int8_t)sda;
    c.i2c_scl = (int8_t)scl;
    c.onewire = (int8_t)ow;

    why = sa_config_validate(&c);
    if (why) {
        return webui_send_error(req, "400 Bad Request", why);
    }
    if (sa_config_save(&c) != ESP_OK) {
        return webui_send_error(req, "500 Internal Server Error", "Saving failed");
    }
    ESP_LOGI(TAG, "settings changed from the web UI, rebooting");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "reboot", true);
    esp_err_t err = webui_send_json(req, o);
    reboot_soon();
    return err;
}

static esp_err_t api_log(httpd_req_t *req)
{
    sa_log_entry_t *e = malloc(sizeof(sa_log_entry_t) * SA_LOG_ENTRIES);
    if (!e) {
        return webui_send_error(req, "500 Internal Server Error", "Out of memory");
    }
    int n = sa_log_get(e, SA_LOG_ENTRIES);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(root, "count", sa_log_count());
    cJSON *arr = cJSON_AddArrayToObject(root, "entries");
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "uptime_ms", (double)e[i].uptime_ms);
        if (e[i].epoch_s) {
            cJSON_AddNumberToObject(it, "time", (double)e[i].epoch_s);
        }
        char lvl[2] = {e[i].level, 0};
        cJSON_AddStringToObject(it, "level", lvl);
        cJSON_AddStringToObject(it, "text", e[i].text);
        cJSON_AddItemToArray(arr, it);
    }
    free(e);
    return webui_send_json(req, root);
}

static esp_err_t api_reboot(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    esp_err_t err = webui_send_json(req, o);
    reboot_soon();
    return err;
}

// ---------------------------------------------------------------- server

esp_err_t webui_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    httpd_handle_t srv = NULL;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = send_asset, .user_ctx = (void *)&s_index},
        {.uri = "/index.html", .method = HTTP_GET, .handler = send_asset, .user_ctx = (void *)&s_index},
        {.uri = "/app.js", .method = HTTP_GET, .handler = send_asset, .user_ctx = (void *)&s_app},
        {.uri = "/style.css", .method = HTTP_GET, .handler = send_asset, .user_ctx = (void *)&s_css},
        {.uri = "/api/status", .method = HTTP_GET, .handler = api_status},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = api_settings_get},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = api_settings_post},
        {.uri = "/api/log", .method = HTTP_GET, .handler = api_log},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = api_reboot},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(srv, &uris[i]));
    }
    webui_register_sensors(srv);
    webui_register_actors(srv);
    ESP_LOGI(TAG, "web UI running on port %d", cfg.server_port);
    return ESP_OK;
}
