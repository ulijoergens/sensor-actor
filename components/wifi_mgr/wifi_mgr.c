// WiFi manager, see wifi_mgr.h.
//
// All state changes happen in the WiFi/IP event handlers (default event loop task).
// The retry timer only calls esp_wifi_connect(). Status readers copy the state under a
// spinlock.
#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "sa_captive.h"
#include "sa_config.h"

static const char *TAG = "wifi";

#ifdef CONFIG_SA_WIFI_TEST_TIMING
static const wifi_timing_t s_timing = {
    .fast_interval_ms = 2000, .fast_period_ms = 20000,
    .slow_interval_ms = 10000, .slow_period_ms = 40000,
    .ap_retry_ms = 30000,
};
#else
static const wifi_timing_t s_timing = {
    .fast_interval_ms = 10 * 1000, .fast_period_ms = 5 * 60 * 1000,
    .slow_interval_ms = 5 * 60 * 1000, .slow_period_ms = 60 * 60 * 1000,
    .ap_retry_ms = 15 * 60 * 1000,
};
#endif

const wifi_timing_t *wifi_mgr_timing(void)
{
    return &s_timing;
}

int64_t wifi_next_retry_ms(int64_t lost_for_ms, const wifi_timing_t *t)
{
    if (lost_for_ms < 0) {
        lost_for_ms = 0;
    }
    if (lost_for_ms < t->fast_period_ms) {
        return t->fast_interval_ms;
    }
    if (lost_for_ms < (int64_t)t->fast_period_ms + t->slow_period_ms) {
        return t->slow_interval_ms;
    }
    return -1;
}

const char *wifi_mgr_state_name(wifi_mgr_state_t s)
{
    switch (s) {
    case WIFI_MGR_CONNECTING: return "connecting";
    case WIFI_MGR_CONNECTED: return "connected";
    case WIFI_MGR_RECONNECTING: return "reconnecting";
    case WIFI_MGR_AP: return "access point";
    default: return "off";
    }
}

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_mgr_state_t s_state;
static bool s_ap_active;
static bool s_have_ssid;
static int64_t s_lost_at_us;       // when the connection was lost (or first attempt)
static int64_t s_next_at_us;       // planned next attempt, 0 = none
static int64_t s_attempt_at_us;    // when the current attempt started
static uint32_t s_attempts;
static esp_ip4_addr_t s_ip;
static char s_ssid[33];
static char s_ap_ssid[33];
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_retry_timer;

static void set_state(wifi_mgr_state_t st)
{
    portENTER_CRITICAL(&s_lock);
    s_state = st;
    portEXIT_CRITICAL(&s_lock);
}

static void retry_cb(void *arg)
{
    portENTER_CRITICAL(&s_lock);
    s_attempts++;
    s_next_at_us = 0;
    s_attempt_at_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

static void schedule_retry(int64_t delay_ms)
{
    esp_timer_stop(s_retry_timer);
    portENTER_CRITICAL(&s_lock);
    s_next_at_us = esp_timer_get_time() + delay_ms * 1000;
    portEXIT_CRITICAL(&s_lock);
    ESP_ERROR_CHECK(esp_timer_start_once(s_retry_timer, delay_ms * 1000));
}

static void start_ap(void)
{
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(s_have_ssid ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    set_state(WIFI_MGR_AP);
    ESP_LOGW(TAG, "starting access point \"%s\" (http://192.168.4.1)", s_ap_ssid);
    if (s_have_ssid) {
        schedule_retry(s_timing.ap_retry_ms);
    }
}

static void stop_ap(void)
{
    if (s_ap_active) {
        ESP_LOGI(TAG, "connected again, stopping access point");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (s_have_ssid && s_state == WIFI_MGR_CONNECTING) {
            portENTER_CRITICAL(&s_lock);
            s_attempts = 1;
            s_attempt_at_us = esp_timer_get_time();
            portEXIT_CRITICAL(&s_lock);
            esp_wifi_connect();
        }
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;
        int64_t now = esp_timer_get_time();
        bool just_lost = s_state == WIFI_MGR_CONNECTED;
        if (just_lost) {
            if (d->reason == WIFI_REASON_ASSOC_LEAVE) {
                ESP_LOGI(TAG, "left \"%s\" (restart or WiFi stopped)", s_ssid);
            } else {
                ESP_LOGW(TAG, "connection to \"%s\" lost (reason %d)", s_ssid, d->reason);
            }
            portENTER_CRITICAL(&s_lock);
            s_lost_at_us = now;
            s_attempts = 0;
            s_ip.addr = 0;
            s_state = WIFI_MGR_RECONNECTING;
            portEXIT_CRITICAL(&s_lock);
        } else {
            ESP_LOGI(TAG, "attempt %lu to \"%s\" failed (reason %d)",
                     (unsigned long)s_attempts, s_ssid, d->reason);
        }
        int64_t delay = s_state == WIFI_MGR_AP
                            ? (int64_t)s_timing.ap_retry_ms
                            : wifi_next_retry_ms((now - s_lost_at_us) / 1000, &s_timing);
        if (delay > 0 && !just_lost) {
            // Intervals run from the start of one attempt to the start of the next; a
            // failed attempt itself takes a few seconds.
            delay -= (now - s_attempt_at_us) / 1000;
            if (delay < 100) {
                delay = 100;
            }
        }
        if (s_state == WIFI_MGR_AP) {
            schedule_retry(delay);
        } else if (delay < 0) {
            ESP_LOGW(TAG, "no connection for %lld min, switching to access point mode",
                     (now - s_lost_at_us) / 60000000);
            start_ap();
        } else {
            schedule_retry(delay);
        }
        break;
    }

    case WIFI_EVENT_AP_START: {
        s_ap_active = true;
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK) {
            sa_captive_start(info.ip.addr);   // REQ-WIFI-4: captive portal while the AP is up
        }
        break;
    }

    case WIFI_EVENT_AP_STOP:
        s_ap_active = false;
        sa_captive_stop();
        break;

    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "client joined the access point");
        break;

    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        esp_timer_stop(s_retry_timer);
        portENTER_CRITICAL(&s_lock);
        s_ip = e->ip_info.ip;
        s_state = WIFI_MGR_CONNECTED;
        s_attempts = 0;
        s_next_at_us = 0;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "connected to \"%s\", IP " IPSTR, s_ssid, IP2STR(&e->ip_info.ip));
        stop_ap();
    }
}

// The node is mains-powered and must answer ARP and HTTP while idle. With modem sleep
// (the default) some access points / mesh repeaters fail to deliver ARP requests, and the
// node becomes unreachable until it sends traffic itself.
static esp_err_t start_wifi(void)
{
    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
    return err;
}

static void make_hostname(char *out, size_t len, const char *name)
{
    size_t j = 0;
    for (const char *p = name; *p && j + 1 < len; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
        } else if (j > 0 && out[j - 1] != '-') {
            out[j++] = '-';
        }
    }
    while (j > 0 && out[j - 1] == '-') {
        j--;
    }
    out[j] = '\0';
    if (j == 0) {
        strlcpy(out, "sensact", len);
    }
}

esp_err_t wifi_mgr_start(void)
{
    const sa_device_cfg_t *cfg = sa_config_get();
    strlcpy(s_ssid, cfg->wifi_ssid, sizeof(s_ssid));
    s_have_ssid = s_ssid[0] != '\0';

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "SensAct-%02X%02X", mac[4], mac[5]);

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    // DHCP option 114 (RFC 8910): newer phones open the portal without probing first.
    static char portal_uri[] = SA_CAPTIVE_URL;
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, portal_uri, strlen(portal_uri));
    esp_netif_dhcps_start(s_ap_netif);
    char host[33];
    make_hostname(host, sizeof(host), cfg->name);
    esp_netif_set_hostname(s_sta_netif, host);

    const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   // credentials live in sa_config
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL));

    if (!s_have_ssid) {
        ESP_LOGW(TAG, "no WiFi network configured");
        start_ap();
        return start_wifi();
    }

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, cfg->wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, cfg->wifi_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = cfg->wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    s_lost_at_us = esp_timer_get_time();   // the retry schedule also applies at boot
    set_state(WIFI_MGR_CONNECTING);
    ESP_LOGI(TAG, "connecting to \"%s\" as %s", s_ssid, host);
    return start_wifi();
}

void wifi_mgr_get_status(wifi_mgr_status_t *out)
{
    memset(out, 0, sizeof(*out));
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    out->state = s_state;
    out->ap_active = s_ap_active;
    out->attempts = s_attempts;
    esp_ip4_addr_t ip = s_ip;
    int64_t lost_at = s_lost_at_us, next_at = s_next_at_us;
    portEXIT_CRITICAL(&s_lock);

    strlcpy(out->ssid, s_ssid, sizeof(out->ssid));
    strlcpy(out->ap_ssid, s_ap_ssid, sizeof(out->ap_ssid));
    if (out->state == WIFI_MGR_CONNECTED) {
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
        }
    } else if (s_have_ssid && out->state != WIFI_MGR_OFF) {
        out->lost_for_s = (uint32_t)((now - lost_at) / 1000000);
    }
    if (next_at > now) {
        out->next_retry_s = (uint32_t)((next_at - now + 999999) / 1000000);
    }
    if (out->ap_active && s_ap_netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK) {
            snprintf(out->ap_ip, sizeof(out->ap_ip), IPSTR, IP2STR(&info.ip));
        }
    }
}

bool wifi_mgr_is_up(void)
{
    return s_state == WIFI_MGR_CONNECTED || s_ap_active;
}
