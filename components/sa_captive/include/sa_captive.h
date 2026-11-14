// Captive portal for the access point (docs/FSD.md REQ-WIFI-4).
//
// While the access point is up, a small DNS server answers every name lookup with the
// AP address (192.168.4.1). Phones and laptops then reach the node with their
// connectivity check and open the portal page, which the web UI redirects to Settings.
// The web UI side is in webui (sa_captive_redirect()).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SA_CAPTIVE_URL "http://192.168.4.1/#/settings"

// Builds the DNS reply for one query (pure, unit-tested). `ip` is in network byte order
// as stored in esp_ip4_addr_t. A/ANY queries for class IN get one A record with `ip`;
// other types get an empty NOERROR answer. Returns the reply length, or 0 to drop the
// packet (not a standard query, malformed, or does not fit).
size_t sa_dns_reply(const uint8_t *query, size_t qlen, uint8_t *out, size_t outmax, uint32_t ip);

// Starts/stops the DNS server task; wifi_mgr calls these on AP start/stop.
void sa_captive_start(uint32_t ap_ip);
void sa_captive_stop(void);
bool sa_captive_active(void);

// For web UI handlers: if the portal is active and the request is a connectivity check
// or for a foreign host, answer with a redirect to the settings page and return true.
bool sa_captive_redirect(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
