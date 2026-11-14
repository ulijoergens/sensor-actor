// Captive portal: DNS server and HTTP redirect, see sa_captive.h.
#include "sa_captive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_netif_ip_addr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "captive";

#define DNS_PORT  53
#define DNS_MAX   512          // classic DNS over UDP
#define TTL_S     60

// ---------------------------------------------------------------- DNS reply (pure)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }

size_t sa_dns_reply(const uint8_t *q, size_t qlen, uint8_t *out, size_t outmax, uint32_t ip)
{
    if (qlen < 12 || qlen > DNS_MAX) {
        return 0;
    }
    uint16_t flags = rd16(q + 2);
    if ((flags & 0x8000) || ((flags >> 11) & 0xF) != 0) {
        return 0;   // a response, or not a standard query
    }
    if (rd16(q + 4) < 1) {
        return 0;   // no question
    }
    // First question: QNAME (labels, no compression in queries), QTYPE, QCLASS
    size_t p = 12;
    while (p < qlen && q[p] != 0) {
        if ((q[p] & 0xC0) != 0) {
            return 0;   // compression pointer or reserved bits in a query: refuse
        }
        p += 1 + q[p];
    }
    if (p + 5 > qlen) {
        return 0;   // truncated
    }
    size_t qend = p + 5;   // zero byte + QTYPE + QCLASS
    uint16_t qtype = rd16(q + p + 1), qclass = rd16(q + p + 3);
    bool answer = (qtype == 1 || qtype == 255) && qclass == 1;   // A or ANY, class IN

    size_t len = qend + (answer ? 16 : 0);
    if (len > outmax) {
        return 0;
    }
    memcpy(out, q, qend);                              // header + question
    wr16(out + 2, 0x8400 | (flags & 0x0100));          // QR, AA, copy RD; RCODE 0
    wr16(out + 4, 1);                                  // QDCOUNT (only the first question)
    wr16(out + 6, answer ? 1 : 0);                     // ANCOUNT
    wr16(out + 8, 0);                                  // NSCOUNT
    wr16(out + 10, 0);                                 // ARCOUNT
    if (answer) {
        uint8_t *a = out + qend;
        wr16(a, 0xC00C);                               // name: pointer to the question
        wr16(a + 2, 1);                                // TYPE A
        wr16(a + 4, 1);                                // CLASS IN
        a[6] = 0; a[7] = 0; wr16(a + 8, TTL_S);        // TTL
        wr16(a + 10, 4);                               // RDLENGTH
        memcpy(a + 12, &ip, 4);                        // already network byte order
    }
    return len;
}

// ---------------------------------------------------------------- DNS server task

static volatile int s_sock = -1;
static volatile bool s_run;
static uint32_t s_ip;

static void dns_task(void *arg)
{
    uint8_t *buf = malloc(DNS_MAX), *out = malloc(DNS_MAX);
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(DNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (!buf || !out || sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "DNS server could not start");
        if (sock >= 0) {
            close(sock);
        }
        free(buf);
        free(out);
        s_run = false;
        vTaskDelete(NULL);
        return;
    }
    s_sock = sock;
    ESP_LOGI(TAG, "captive portal active: all names resolve to " IPSTR, IP2STR((esp_ip4_addr_t *)&s_ip));
    while (s_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, DNS_MAX, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            continue;   // closed by sa_captive_stop() or a transient error
        }
        size_t r = sa_dns_reply(buf, (size_t)n, out, DNS_MAX, s_ip);
        if (r) {
            sendto(sock, out, r, 0, (struct sockaddr *)&from, fl);
        }
    }
    close(sock);
    s_sock = -1;
    free(buf);
    free(out);
    ESP_LOGI(TAG, "captive portal stopped");
    vTaskDelete(NULL);
}

void sa_captive_start(uint32_t ap_ip)
{
    if (s_run) {
        return;
    }
    s_ip = ap_ip;
    s_run = true;
    if (xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL) != pdPASS) {
        s_run = false;
        ESP_LOGE(TAG, "DNS task could not start");
    }
}

void sa_captive_stop(void)
{
    if (!s_run) {
        return;
    }
    s_run = false;
    int sock = s_sock;
    if (sock >= 0) {
        shutdown(sock, SHUT_RDWR);   // wakes recvfrom(); the task closes the socket
    }
}

bool sa_captive_active(void)
{
    return s_run;
}

// ---------------------------------------------------------------- HTTP redirect

bool sa_captive_redirect(httpd_req_t *req)
{
    if (!s_run) {
        return false;
    }
    char host[64] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    char own[16];
    snprintf(own, sizeof(own), IPSTR, IP2STR((esp_ip4_addr_t *)&s_ip));
    // Requests for the node itself are served normally; anything for another host name
    // (connectivity checks such as connectivitycheck.gstatic.com/generate_204,
    // captive.apple.com/hotspot-detect.html, www.msftconnecttest.com/connecttest.txt)
    // is sent to the settings page.
    if (host[0] && strncmp(host, own, strlen(own)) == 0) {
        return false;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", SA_CAPTIVE_URL);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return true;
}
