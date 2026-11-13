// WiFi manager (docs/FSD.md §6, REQ-WIFI-1..3).
//
// Station mode with the configured network. When the connection is lost (or cannot be
// made at boot) it retries every 10 s for 5 min, then every 5 min for 1 h, then starts
// an access point "SensAct-XXXX" (192.168.4.1) and keeps retrying the configured
// network every 15 min. Without configured credentials it starts the AP directly.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t fast_interval_ms;   // 10 s
    uint32_t fast_period_ms;     // 5 min
    uint32_t slow_interval_ms;   // 5 min
    uint32_t slow_period_ms;     // 60 min (after the fast period)
    uint32_t ap_retry_ms;        // 15 min (REQ-WIFI-2)
} wifi_timing_t;

// Timing in use: the FSD values, or short test values with CONFIG_SA_WIFI_TEST_TIMING.
const wifi_timing_t *wifi_mgr_timing(void);

// Pure schedule (unit-tested): delay before the next attempt, given how long the
// connection has been lost; -1 means "give up and start the access point".
int64_t wifi_next_retry_ms(int64_t lost_for_ms, const wifi_timing_t *t);

typedef enum {
    WIFI_MGR_OFF = 0,
    WIFI_MGR_CONNECTING,     // first connection attempt after boot
    WIFI_MGR_CONNECTED,
    WIFI_MGR_RECONNECTING,   // lost, retrying on the schedule
    WIFI_MGR_AP,             // access point active (retrying every 15 min if configured)
} wifi_mgr_state_t;

typedef struct {
    wifi_mgr_state_t state;
    bool ap_active;
    char ssid[33];           // configured network
    char ap_ssid[33];
    char ip[16];             // station IP when connected
    char ap_ip[16];
    int8_t rssi;             // when connected
    uint32_t lost_for_s;     // RECONNECTING / AP: time since the connection was lost
    uint32_t next_retry_s;   // seconds until the next attempt (0 = none planned)
    uint32_t attempts;       // attempts since the connection was lost
} wifi_mgr_status_t;

const char *wifi_mgr_state_name(wifi_mgr_state_t s);

// Starts WiFi with the settings from sa_config. Needs nvs_flash_init(),
// esp_netif_init() and the default event loop.
esp_err_t wifi_mgr_start(void);

void wifi_mgr_get_status(wifi_mgr_status_t *out);

// true once the node is reachable: connected as station or access point up.
bool wifi_mgr_is_up(void);

#ifdef __cplusplus
}
#endif
