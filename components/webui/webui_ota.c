// Firmware update over the web UI (docs/FSD.md §8, REQ-OTA-1/2):
//   GET  /api/ota   running slot, other slot, versions, rollback state
//   POST /api/ota   raw firmware image (build/sensor-actor.bin) as the request body
//
// The image is streamed into the inactive OTA slot (erased as it is written), checked
// (ESP-IDF image validation plus project name), marked for the next boot, and the node
// restarts. The new firmware must confirm itself within 60 s after boot (see main), or
// the bootloader rolls back to the previous one.
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "webui_priv.h"

static const char *TAG = "ota";

#define CHUNK 4096
#define DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;

static const char *state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "unknown";
    }
}

static cJSON *slot_json(const esp_partition_t *p)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "label", p ? p->label : "");
    esp_app_desc_t d;
    if (p && esp_ota_get_partition_description(p, &d) == ESP_OK) {
        cJSON_AddStringToObject(o, "version", d.version);
        cJSON_AddStringToObject(o, "project", d.project_name);
        char built[40];
        snprintf(built, sizeof(built), "%s %s", d.date, d.time);
        cJSON_AddStringToObject(o, "built", built);
    }
    esp_ota_img_states_t st;
    if (p && esp_ota_get_state_partition(p, &st) == ESP_OK) {
        cJSON_AddStringToObject(o, "state", state_name(st));
    }
    return o;
}

static esp_err_t api_ota_get(httpd_req_t *req)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "running", slot_json(run));
    cJSON_AddItemToObject(o, "other", slot_json(next));
    cJSON_AddNumberToObject(o, "max_size", next ? next->size : 0);
    if (bad) {
        cJSON_AddItemToObject(o, "last_invalid", slot_json(bad));
    }
    cJSON_AddBoolToObject(o, "busy", s_busy);
    return webui_send_json(req, o);
}

static void reboot_cb(void *arg)
{
    esp_restart();
}

// Reads exactly `len` bytes of the request body (fewer only at the end of the body).
static int recv_full(httpd_req_t *req, char *buf, int len)
{
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;   // slow uploads over WiFi: keep waiting
        }
        if (r <= 0) {
            return got > 0 ? got : r;
        }
        got += r;
    }
    return got;
}

static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, const char *status, const char *msg)
{
    if (h) {
        esp_ota_abort(h);
    }
    ESP_LOGW(TAG, "firmware upload rejected: %s", msg);
    portENTER_CRITICAL(&s_lock);
    s_busy = false;
    portEXIT_CRITICAL(&s_lock);
    return webui_send_error(req, status, msg);
}

static esp_err_t api_ota_post(httpd_req_t *req)
{
    portENTER_CRITICAL(&s_lock);
    bool busy = s_busy;
    s_busy = true;
    portEXIT_CRITICAL(&s_lock);
    if (busy) {
        return webui_send_error(req, "409 Conflict", "Another firmware upload is in progress");
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return fail(req, 0, "500 Internal Server Error", "No OTA slot available");
    }
    if (req->content_len < (int)(DESC_OFFSET + sizeof(esp_app_desc_t))) {
        return fail(req, 0, "400 Bad Request", "No firmware file in the request (too small)");
    }
    if (req->content_len > (int)target->size) {
        return fail(req, 0, "400 Bad Request", "Firmware file is larger than the firmware slot");
    }

    char *buf = malloc(CHUNK);
    if (!buf) {
        return fail(req, 0, "500 Internal Server Error", "Out of memory");
    }
    esp_ota_handle_t h = 0;
    int total = 0;
    esp_err_t err = ESP_OK;
    const char *why = NULL;
    char version[32] = "";

    while (total < req->content_len) {
        int want = req->content_len - total < CHUNK ? req->content_len - total : CHUNK;
        int n = recv_full(req, buf, want);
        if (n <= 0) {
            why = "Upload interrupted";
            break;
        }
        if (total == 0) {
            // First chunk: check that this is an ESP32 application image of this project
            // before touching the flash.
            const esp_image_header_t *hdr = (const esp_image_header_t *)buf;
            const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + DESC_OFFSET);
            if (n < (int)(DESC_OFFSET + sizeof(esp_app_desc_t)) || hdr->magic != ESP_IMAGE_HEADER_MAGIC ||
                desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
                why = "Not an ESP32 firmware image (use build/sensor-actor.bin)";
                break;
            }
            if (hdr->chip_id != ESP_CHIP_ID_ESP32) {
                why = "Firmware is built for a different chip";
                break;
            }
            if (strncmp(desc->project_name, esp_app_get_description()->project_name, sizeof(desc->project_name)) != 0) {
                why = "Firmware is from a different project";
                break;
            }
            strlcpy(version, desc->version, sizeof(version));
            ESP_LOGI(TAG, "receiving firmware %s (%d bytes) into %s", version, req->content_len, target->label);
            err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &h);
            if (err != ESP_OK) {
                why = "Could not start the update";
                break;
            }
        }
        err = esp_ota_write(h, buf, n);
        if (err != ESP_OK) {
            why = "Writing to flash failed";
            break;
        }
        total += n;
    }
    free(buf);
    if (why) {
        return fail(req, h, "400 Bad Request", why);
    }

    err = esp_ota_end(h);   // validates the image (segments, checksum, SHA-256)
    h = 0;
    if (err != ESP_OK) {
        return fail(req, 0, "400 Bad Request",
                    err == ESP_ERR_OTA_VALIDATE_FAILED ? "Firmware image is damaged (validation failed)" : "Finishing the update failed");
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return fail(req, 0, "500 Internal Server Error", "Could not select the new firmware");
    }
    ESP_LOGW(TAG, "firmware %s installed in %s, restarting", version, target->label);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "version", version);
    cJSON_AddStringToObject(o, "slot", target->label);
    cJSON_AddNumberToObject(o, "size", total);
    esp_err_t ret = webui_send_json(req, o);

    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = {.callback = reboot_cb, .name = "ota_reboot"};
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 1500 * 1000);
    return ret;
}

void webui_register_ota(httpd_handle_t srv)
{
    const httpd_uri_t uris[] = {
        {.uri = "/api/ota", .method = HTTP_GET, .handler = api_ota_get},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = api_ota_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        webui_register(srv, &uris[i]);
    }
}
