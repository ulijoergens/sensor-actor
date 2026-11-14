// Crash report API:
//   GET    /api/crash        summary {present, task, pc, reason, backtrace[], size}
//   GET    /api/crash/dump   raw core dump (ELF) for `idf.py coredump-info -c <file>`
//   DELETE /api/crash        erase the stored dump
#include <stdio.h>
#include <stdlib.h>
#include "sa_crash.h"
#include "webui_priv.h"

static esp_err_t api_crash(httpd_req_t *req)
{
    sa_crash_info_t c;
    sa_crash_get(&c);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "present", c.present);
    if (c.present) {
        char hex[12];
        cJSON_AddStringToObject(o, "task", c.task);
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)c.pc);
        cJSON_AddStringToObject(o, "pc", hex);
        cJSON_AddStringToObject(o, "reason", c.reason);
        cJSON_AddNumberToObject(o, "exc_cause", c.exc_cause);
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)c.exc_vaddr);
        cJSON_AddStringToObject(o, "exc_vaddr", hex);
        cJSON *bt = cJSON_AddArrayToObject(o, "backtrace");
        for (uint32_t i = 0; i < c.bt_depth; i++) {
            snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)c.bt[i]);
            cJSON_AddItemToArray(bt, cJSON_CreateString(hex));
        }
        cJSON_AddBoolToObject(o, "backtrace_corrupted", c.bt_corrupted);
        cJSON_AddNumberToObject(o, "size", c.size);
        cJSON_AddStringToObject(o, "elf_sha256", c.elf_sha256);
    }
    return webui_send_json(req, o);
}

static esp_err_t api_crash_dump(httpd_req_t *req)
{
    sa_crash_info_t c;
    sa_crash_get(&c);
    if (!c.present) {
        return webui_send_error(req, "404 Not Found", "No crash report stored");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.elf\"");
    char *buf = malloc(1024);
    if (!buf) {
        return webui_send_error(req, "500 Internal Server Error", "Out of memory");
    }
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < c.size && err == ESP_OK; off += 1024) {
        size_t n = c.size - off < 1024 ? c.size - off : 1024;
        err = sa_crash_read(off, buf, n);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, buf, n);
        }
    }
    free(buf);
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t api_crash_delete(httpd_req_t *req)
{
    if (sa_crash_erase() != ESP_OK) {
        return webui_send_error(req, "500 Internal Server Error", "Erasing failed");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return webui_send_json(req, o);
}

void webui_register_crash(httpd_handle_t srv)
{
    const httpd_uri_t uris[] = {
        {.uri = "/api/crash", .method = HTTP_GET, .handler = api_crash},
        {.uri = "/api/crash/dump", .method = HTTP_GET, .handler = api_crash_dump},
        {.uri = "/api/crash", .method = HTTP_DELETE, .handler = api_crash_delete},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        webui_register(srv, &uris[i]);
    }
}
