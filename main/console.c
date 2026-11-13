// Serial console for provisioning and bench testing (115200 baud on UART0).
//
//   status                      device, WiFi and log summary
//   show                        current settings (passwords hidden)
//   set <key> <value...>        change one setting and save it; most need a reboot
//   log                         warnings and errors, newest first
//   net | ping <ip> [count]     network diagnostics
//   reboot | factory-reset
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "ping/ping_sock.h"
#include "sa_config.h"
#include "sa_log.h"
#include "wifi_mgr.h"

static int cmd_status(int argc, char **argv)
{
    const sa_device_cfg_t *c = sa_config_get();
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    printf("device   %s (%s), firmware %s, uptime %llds, free heap %lu\n", c->name, c->id,
           esp_app_get_description()->version, esp_timer_get_time() / 1000000,
           (unsigned long)esp_get_free_heap_size());
    printf("wifi     %s, network \"%s\"", wifi_mgr_state_name(w.state), w.ssid);
    if (w.state == WIFI_MGR_CONNECTED) {
        printf(", IP %s, RSSI %d dBm", w.ip, w.rssi);
    } else if (w.lost_for_s) {
        printf(", lost for %lus, attempts %lu", (unsigned long)w.lost_for_s, (unsigned long)w.attempts);
    }
    if (w.next_retry_s) {
        printf(", next attempt in %lus", (unsigned long)w.next_retry_s);
    }
    printf("\n");
    if (w.ap_active) {
        printf("ap       \"%s\" at %s\n", w.ap_ssid, w.ap_ip);
    }
    printf("log      %lu warnings/errors since boot\n", (unsigned long)sa_log_count());
    return 0;
}

static int cmd_show(int argc, char **argv)
{
    const sa_device_cfg_t *c = sa_config_get();
    printf("name       %s\nid         %s\nssid       %s\npass       %s\n", c->name, c->id,
           c->wifi_ssid, c->wifi_pass[0] ? "(set)" : "(empty)");
    printf("mqtt_host  %s\nmqtt_port  %u\nmqtt_user  %s\nmqtt_pass  %s\n", c->mqtt_host,
           c->mqtt_port, c->mqtt_user, c->mqtt_pass[0] ? "(set)" : "(empty)");
    printf("discovery  %s\nprefix     %s\nsda        %d\nscl        %d\nonewire    %d\n",
           c->discovery ? "on" : "off", c->discovery_prefix, c->i2c_sda, c->i2c_scl, c->onewire);
    return 0;
}

static int set_str(char *dst, size_t len, const char *v)
{
    if (strlen(v) >= len) {
        printf("value too long (max %u characters)\n", (unsigned)(len - 1));
        return 1;
    }
    strcpy(dst, v);
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: set <key> [value...]   keys: name id ssid pass mqtt_host mqtt_port "
               "mqtt_user mqtt_pass discovery prefix sda scl onewire\n");
        return 1;
    }
    // Join the remaining arguments so values may contain spaces (e.g. SSIDs).
    char val[80] = "";
    for (int i = 2; i < argc; i++) {
        if (strlen(val) + strlen(argv[i]) + 2 > sizeof(val)) {
            printf("value too long\n");
            return 1;
        }
        if (i > 2) {
            strcat(val, " ");
        }
        strcat(val, argv[i]);
    }
    sa_device_cfg_t c = *sa_config_get();
    const char *k = argv[1];
    int err = 0;
    if (!strcmp(k, "name")) err = set_str(c.name, sizeof(c.name), val);
    else if (!strcmp(k, "id")) err = set_str(c.id, sizeof(c.id), val);
    else if (!strcmp(k, "ssid")) err = set_str(c.wifi_ssid, sizeof(c.wifi_ssid), val);
    else if (!strcmp(k, "pass")) err = set_str(c.wifi_pass, sizeof(c.wifi_pass), val);
    else if (!strcmp(k, "mqtt_host")) err = set_str(c.mqtt_host, sizeof(c.mqtt_host), val);
    else if (!strcmp(k, "mqtt_user")) err = set_str(c.mqtt_user, sizeof(c.mqtt_user), val);
    else if (!strcmp(k, "mqtt_pass")) err = set_str(c.mqtt_pass, sizeof(c.mqtt_pass), val);
    else if (!strcmp(k, "prefix")) err = set_str(c.discovery_prefix, sizeof(c.discovery_prefix), val);
    else if (!strcmp(k, "mqtt_port")) c.mqtt_port = (uint16_t)atoi(val);
    else if (!strcmp(k, "discovery")) c.discovery = !strcmp(val, "on") || !strcmp(val, "1");
    else if (!strcmp(k, "sda")) c.i2c_sda = (int8_t)atoi(val);
    else if (!strcmp(k, "scl")) c.i2c_scl = (int8_t)atoi(val);
    else if (!strcmp(k, "onewire")) c.onewire = (int8_t)atoi(val);
    else {
        printf("unknown key \"%s\"\n", k);
        return 1;
    }
    if (err) {
        return 1;
    }
    const char *why = sa_config_validate(&c);
    if (why) {
        printf("not saved: %s\n", why);
        return 1;
    }
    if (sa_config_save(&c) != ESP_OK) {
        printf("saving failed\n");
        return 1;
    }
    printf("saved; type \"reboot\" to apply\n");
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    static sa_log_entry_t e[SA_LOG_ENTRIES];
    int n = sa_log_get(e, SA_LOG_ENTRIES);
    for (int i = 0; i < n; i++) {
        printf("%c %8.1fs  %s\n", e[i].level, e[i].uptime_ms / 1000.0, e[i].text);
    }
    if (n == 0) {
        printf("no warnings or errors\n");
    }
    return 0;
}

static int cmd_net(int argc, char **argv)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
        printf("ip       " IPSTR "\nnetmask  " IPSTR "\ngateway  " IPSTR "\n", IP2STR(&ip.ip),
               IP2STR(&ip.netmask), IP2STR(&ip.gw));
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            printf("dns      " IPSTR "\n", IP2STR(&dns.ip.u_addr.ip4));
        }
    }
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        printf("mac      " MACSTR "\n", MAC2STR(mac));
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        printf("ap       \"%s\" bssid " MACSTR ", channel %d, RSSI %d dBm\n", (const char *)ap.ssid,
               MAC2STR(ap.bssid), ap.primary, ap.rssi);
    } else {
        printf("ap       not connected\n");
    }
    return 0;
}

static SemaphoreHandle_t s_ping_done;

static void ping_ok(esp_ping_handle_t h, void *arg)
{
    uint32_t seq, ms;
    ip_addr_t from;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    esp_ping_get_profile(h, ESP_PING_PROF_IPADDR, &from, sizeof(from));
    printf("reply from %s: seq=%lu time=%lu ms\n", ipaddr_ntoa(&from), (unsigned long)seq, (unsigned long)ms);
}

static void ping_timeout(esp_ping_handle_t h, void *arg)
{
    uint32_t seq;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    printf("timeout: seq=%lu\n", (unsigned long)seq);
}

static void ping_end(esp_ping_handle_t h, void *arg)
{
    uint32_t sent, got;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &got, sizeof(got));
    printf("%lu sent, %lu received\n", (unsigned long)sent, (unsigned long)got);
    xSemaphoreGive(s_ping_done);
}

static int cmd_ping(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: ping <ipv4-address> [count]\n");
        return 1;
    }
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    if (!ipaddr_aton(argv[1], &pc.target_addr)) {
        printf("not an IP address: %s\n", argv[1]);
        return 1;
    }
    pc.count = argc > 2 ? (uint32_t)atoi(argv[2]) : 4;
    if (pc.count == 0 || pc.count > 20) {
        pc.count = 4;
    }
    if (!s_ping_done) {
        s_ping_done = xSemaphoreCreateBinary();
    }
    esp_ping_callbacks_t cbs = {.on_ping_success = ping_ok, .on_ping_timeout = ping_timeout,
                                .on_ping_end = ping_end};
    esp_ping_handle_t h;
    if (esp_ping_new_session(&pc, &cbs, &h) != ESP_OK) {
        printf("ping failed to start\n");
        return 1;
    }
    esp_ping_start(h);
    xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(pc.count * 2000 + 2000));
    esp_ping_delete_session(h);
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    printf("rebooting\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    sa_config_factory_reset();
    return cmd_reboot(argc, argv);
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "sa> ";
    rc.max_history_len = 8;
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uc, &rc, &repl));

    const esp_console_cmd_t cmds[] = {
        {.command = "status", .help = "Device, WiFi and log summary", .func = cmd_status},
        {.command = "show", .help = "Show settings (passwords hidden)", .func = cmd_show},
        {.command = "set", .help = "set <key> <value>: change and save one setting", .func = cmd_set},
        {.command = "log", .help = "Warnings and errors, newest first", .func = cmd_log},
        {.command = "net", .help = "IP address, gateway, DNS and access point details", .func = cmd_net},
        {.command = "ping", .help = "ping <ip> [count]: check reachability from the device", .func = cmd_ping},
        {.command = "reboot", .help = "Restart the device", .func = cmd_reboot},
        {.command = "factory-reset", .help = "Erase all settings and restart", .func = cmd_factory_reset},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
