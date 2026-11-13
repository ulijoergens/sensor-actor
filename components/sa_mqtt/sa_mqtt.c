// MQTT client, see sa_mqtt.h.
#include "sa_mqtt.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mqtt_client.h"
#include "sa_config.h"
#include "wifi_mgr.h"

static const char *TAG = "mqtt";

#define INFO_INTERVAL_US (60LL * 1000 * 1000)

static esp_mqtt_client_handle_t s_client;
static esp_timer_handle_t s_info_timer;
static sa_mqtt_command_cb_t s_cmd_cb;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static sa_mqtt_status_t s_st;
static int64_t s_lost_at_us;
static char s_id[SA_ID_MAX + 1];
static char s_status_topic[48];
static bool s_logged_down;   // avoid one log line per reconnect attempt
static bool s_running;       // client started (only while WiFi has an IP address)

// ---------------------------------------------------------------- pure helpers

bool sa_mqtt_topic(char *buf, size_t len, const char *device_id, const char *sub)
{
    int n = snprintf(buf, len, SA_MQTT_BASE "/%s/%s", device_id, sub);
    return n > 0 && (size_t)n < len;
}

bool sa_mqtt_parse_actor_set(const char *topic, size_t topic_len, const char *device_id,
                             char *aid, size_t aid_len)
{
    char prefix[48];
    int n = snprintf(prefix, sizeof(prefix), SA_MQTT_BASE "/%s/actor/", device_id);
    if (n <= 0 || (size_t)n >= sizeof(prefix) || topic_len <= (size_t)n + 4 ||
        strncmp(topic, prefix, n) != 0) {
        return false;
    }
    const char *rest = topic + n;
    size_t rest_len = topic_len - n;
    if (rest_len < 5 || memcmp(rest + rest_len - 4, "/set", 4) != 0) {
        return false;
    }
    size_t id_len = rest_len - 4;
    if (id_len == 0 || id_len >= aid_len || id_len > SA_ID_MAX) {
        return false;
    }
    memcpy(aid, rest, id_len);
    aid[id_len] = '\0';
    return sa_id_valid(aid);
}

// ---------------------------------------------------------------- publishing

bool sa_mqtt_publish(const char *sub, const char *payload, bool retain)
{
    if (!s_client || !s_st.connected) {
        return false;
    }
    char topic[96];
    if (!sa_mqtt_topic(topic, sizeof(topic), s_id, sub)) {
        return false;
    }
    int id = esp_mqtt_client_enqueue(s_client, topic, payload, 0, 1, retain, true);
    if (id < 0) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    s_st.published++;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

static void publish_raw(const char *topic, const char *payload, bool retain)
{
    if (esp_mqtt_client_enqueue(s_client, topic, payload, 0, 1, retain, true) >= 0) {
        portENTER_CRITICAL(&s_lock);
        s_st.published++;
        portEXIT_CRITICAL(&s_lock);
    }
}

static void publish_info(void)
{
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "ip", w.ip);
    cJSON_AddNumberToObject(o, "rssi", w.rssi);
    cJSON_AddNumberToObject(o, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "heap", esp_get_free_heap_size());
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (txt) {
        sa_mqtt_publish("info", txt, true);
        cJSON_free(txt);
    }
}

static void info_timer_cb(void *arg)
{
    publish_info();
}

// ---------------------------------------------------------------- discovery (REQ-MQ-4)

typedef struct {
    const char *key;          // object id suffix
    const char *name;
    const char *component;    // HA component type
    const char *value_tpl;
    const char *unit;
    const char *dev_class;
} diag_entity_t;

static const diag_entity_t s_diag[] = {
    {"rssi", "WiFi signal", "sensor", "{{ value_json.rssi }}", "dBm", "signal_strength"},
    {"uptime", "Uptime", "sensor", "{{ value_json.uptime }}", "s", "duration"},
    {"ip", "IP address", "sensor", "{{ value_json.ip }}", NULL, NULL},
    {"heap", "Free memory", "sensor", "{{ value_json.heap }}", "B", "data_size"},
    {"firmware", "Firmware", "sensor", "{{ value_json.firmware }}", NULL, NULL},
};

static void add_device(cJSON *o, const sa_device_cfg_t *c)
{
    uint8_t mac[6];
    char macs[18], ident[24], url[40];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(macs, sizeof(macs), MACSTR, MAC2STR(mac));
    snprintf(ident, sizeof(ident), "sensact_%s", c->id);

    cJSON *d = cJSON_AddObjectToObject(o, "device");
    cJSON *ids = cJSON_AddArrayToObject(d, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(ident));
    cJSON *conns = cJSON_AddArrayToObject(d, "connections");
    cJSON *conn = cJSON_CreateArray();
    cJSON_AddItemToArray(conn, cJSON_CreateString("mac"));
    cJSON_AddItemToArray(conn, cJSON_CreateString(macs));
    cJSON_AddItemToArray(conns, conn);
    cJSON_AddStringToObject(d, "name", c->name);
    cJSON_AddStringToObject(d, "manufacturer", "sensor-actor");
    cJSON_AddStringToObject(d, "model", "ESP32 sensor-actor node");
    cJSON_AddStringToObject(d, "sw_version", esp_app_get_description()->version);
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    if (w.ip[0]) {
        snprintf(url, sizeof(url), "http://%s/", w.ip);
        cJSON_AddStringToObject(d, "configuration_url", url);
    }
}

static void publish_discovery(void)
{
    const sa_device_cfg_t *c = sa_config_get();
    char topic[128], state_topic[48];
    sa_mqtt_topic(state_topic, sizeof(state_topic), c->id, "info");

    for (size_t i = 0; i < sizeof(s_diag) / sizeof(s_diag[0]); i++) {
        const diag_entity_t *e = &s_diag[i];
        snprintf(topic, sizeof(topic), "%s/%s/sensact_%s_%s/config", c->discovery_prefix,
                 e->component, c->id, e->key);
        if (!c->discovery) {
            publish_raw(topic, "", true);   // remove previously announced entities
            continue;
        }
        char uid[40];
        snprintf(uid, sizeof(uid), "sensact_%s_%s", c->id, e->key);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "unique_id", uid);
        cJSON_AddStringToObject(o, "object_id", uid);
        cJSON_AddStringToObject(o, "state_topic", state_topic);
        cJSON_AddStringToObject(o, "value_template", e->value_tpl);
        if (e->unit) {
            cJSON_AddStringToObject(o, "unit_of_measurement", e->unit);
        }
        if (e->dev_class) {
            cJSON_AddStringToObject(o, "device_class", e->dev_class);
        }
        cJSON_AddStringToObject(o, "entity_category", "diagnostic");
        cJSON_AddStringToObject(o, "availability_topic", s_status_topic);
        add_device(o, c);
        char *txt = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (txt) {
            publish_raw(topic, txt, true);
            cJSON_free(txt);
        }
    }
    ESP_LOGI(TAG, "discovery %s (prefix \"%s\")", c->discovery ? "published" : "removed",
             c->discovery_prefix);
}

// ---------------------------------------------------------------- events

static void set_error(const char *msg)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_st.last_error, msg, sizeof(s_st.last_error));
    portEXIT_CRITICAL(&s_lock);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        portENTER_CRITICAL(&s_lock);
        s_st.connected = true;
        s_st.connects++;
        s_st.last_error[0] = '\0';
        portEXIT_CRITICAL(&s_lock);
        s_logged_down = false;
        ESP_LOGI(TAG, "connected to %s", s_st.broker);
        publish_raw(s_status_topic, "online", true);
        char sub[64];
        snprintf(sub, sizeof(sub), SA_MQTT_BASE "/%s/actor/+/set", s_id);
        esp_mqtt_client_subscribe(ev->client, sub, 1);
        publish_discovery();
        publish_info();
        esp_timer_stop(s_info_timer);
        esp_timer_start_periodic(s_info_timer, INFO_INTERVAL_US);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (s_st.connected) {
            s_lost_at_us = esp_timer_get_time();
        }
        portENTER_CRITICAL(&s_lock);
        s_st.connected = false;
        portEXIT_CRITICAL(&s_lock);
        esp_timer_stop(s_info_timer);
        if (!s_logged_down) {
            ESP_LOGW(TAG, "not connected to %s%s%s, retrying every 10 s", s_st.broker,
                     s_st.last_error[0] ? ": " : "", s_st.last_error);
            s_logged_down = true;
        }
        break;

    case MQTT_EVENT_DATA: {
        char aid[SA_ID_MAX + 1];
        char payload[32];
        int n = ev->data_len < (int)sizeof(payload) - 1 ? ev->data_len : (int)sizeof(payload) - 1;
        memcpy(payload, ev->data, n);
        payload[n] = '\0';
        if (ev->current_data_offset != 0) {
            break;   // fragments of large messages are not commands
        }
        if (!sa_mqtt_parse_actor_set(ev->topic, ev->topic_len, s_id, aid, sizeof(aid))) {
            break;
        }
        if (!s_cmd_cb || !s_cmd_cb(aid, payload)) {
            ESP_LOGW(TAG, "command \"%s\" for unknown or inactive actor \"%s\" ignored", payload, aid);
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        if (ev->error_handle) {
            const esp_mqtt_error_codes_t *e = ev->error_handle;
            char msg[64];
            if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                if (e->esp_transport_sock_errno) {
                    snprintf(msg, sizeof(msg), "network error: %s", strerror(e->esp_transport_sock_errno));
                } else {
                    snprintf(msg, sizeof(msg), "cannot reach the broker (%s)", esp_err_to_name(e->esp_tls_last_esp_err));
                }
            } else if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                snprintf(msg, sizeof(msg), "broker refused the connection (code %d)", e->connect_return_code);
            } else {
                snprintf(msg, sizeof(msg), "error type %d", e->error_type);
            }
            set_error(msg);
        }
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------- WiFi coupling

// The client only runs while the station has an IP address. Otherwise every boot and
// every WiFi outage would produce a burst of connection errors.
static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && !s_running) {
        s_running = true;
        s_logged_down = false;
        ESP_LOGI(TAG, "WiFi up, connecting to %s", s_st.broker);
        esp_mqtt_client_start(s_client);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && s_running) {
        s_running = false;
        esp_mqtt_client_stop(s_client);
        if (s_st.connected) {
            s_lost_at_us = esp_timer_get_time();
        }
        portENTER_CRITICAL(&s_lock);
        s_st.connected = false;
        strlcpy(s_st.last_error, "waiting for WiFi", sizeof(s_st.last_error));
        portEXIT_CRITICAL(&s_lock);
        esp_timer_stop(s_info_timer);
    }
}

// ---------------------------------------------------------------- start / status

esp_err_t sa_mqtt_start(void)
{
    const sa_device_cfg_t *c = sa_config_get();
    strlcpy(s_id, c->id, sizeof(s_id));
    if (!c->mqtt_host[0]) {
        ESP_LOGI(TAG, "no broker configured, MQTT off");
        return ESP_OK;
    }
    s_st.configured = true;
    snprintf(s_st.broker, sizeof(s_st.broker), "%s:%u", c->mqtt_host, c->mqtt_port);
    sa_mqtt_topic(s_status_topic, sizeof(s_status_topic), s_id, "status");

    const esp_timer_create_args_t ta = {.callback = info_timer_cb, .name = "mqtt_info"};
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_info_timer));

    char client_id[24];
    snprintf(client_id, sizeof(client_id), "sensact-%s", s_id);
    esp_mqtt_client_config_t mc = {
        .broker.address.hostname = c->mqtt_host,
        .broker.address.port = c->mqtt_port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id = client_id,
        .credentials.username = c->mqtt_user[0] ? c->mqtt_user : NULL,
        .credentials.authentication.password = c->mqtt_pass[0] ? c->mqtt_pass : NULL,
        .session.last_will = {
            .topic = s_status_topic, .msg = "offline", .msg_len = 7, .qos = 1, .retain = 1,
        },
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 10000,
        .network.timeout_ms = 5000,
        .buffer.size = 1536,   // discovery messages are ~600 bytes
    };
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    s_lost_at_us = esp_timer_get_time();
    strlcpy(s_st.last_error, "waiting for WiFi", sizeof(s_st.last_error));

    // The library's own transport errors repeat on every reconnect attempt and would fill
    // the error log during a broker outage; this module logs one line with the reason.
    esp_log_level_set("mqtt_client", ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    esp_log_level_set("esp-tls", ESP_LOG_NONE);

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_net_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_net_event, NULL));
    ESP_LOGI(TAG, "broker %s, client ID %s", s_st.broker, client_id);

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
        on_net_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);   // already connected
    }
    return ESP_OK;
}

void sa_mqtt_get_status(sa_mqtt_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    int64_t lost = s_lost_at_us;
    portEXIT_CRITICAL(&s_lock);
    out->disconnected_s = (!out->configured || out->connected) ? 0
                          : (uint32_t)((esp_timer_get_time() - lost) / 1000000);
}

void sa_mqtt_set_command_handler(sa_mqtt_command_cb_t cb)
{
    s_cmd_cb = cb;
}
