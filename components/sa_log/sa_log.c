// Ring buffers of warnings and errors, fed by a vprintf hook on ESP-IDF logging, with the
// last SA_LOG_PERSIST entries kept in NVS. See sa_log.h.
#include "sa_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "log";

#define NVS_NS        "salog"     // own namespace: a settings factory reset keeps the log
#define KEY_ENTRIES   "entries"
#define KEY_BOOT      "boot"
#define STORE_VERSION 1

typedef struct {
    uint16_t version;
    uint16_t count;               // valid entries, oldest first
    sa_log_persisted_t e[SA_LOG_PERSIST];
} store_t;

static sa_log_entry_t s_ring[SA_LOG_ENTRIES];
static uint32_t s_total;          // entries ever written; next slot = s_total % SA_LOG_ENTRIES
static store_t s_store;           // persisted entries (all boots), oldest first
static uint32_t s_boot;
static bool s_dirty;
static bool s_persist_ready;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;

void sa_log_clean_line(char *dst, size_t len, const char *src)
{
    const char *p = src;
    if (p[0] == '\033') {                    // leading colour code
        const char *m = strchr(p, 'm');
        p = m ? m + 1 : p;
    }
    if ((p[0] == 'W' || p[0] == 'E' || p[0] == 'I') && p[1] == ' ' && p[2] == '(') {
        const char *q = strchr(p, ')');
        if (q && q[1] == ' ') {
            p = q + 2;
        }
    }
    size_t j = 0;
    while (*p && j + 1 < len) {
        if (*p == '\033') {                  // skip "\033[...m"
            while (*p && *p != 'm') {
                p++;
            }
            if (*p) {
                p++;
            }
            continue;
        }
        if (*p == '\n' || *p == '\r') {
            p++;
            continue;
        }
        dst[j++] = *p++;
    }
    dst[j] = '\0';
}

static void persist_push(const sa_log_entry_t *e)
{
    // called with s_lock held
    if (s_store.count == SA_LOG_PERSIST) {
        memmove(&s_store.e[0], &s_store.e[1], sizeof(s_store.e[0]) * (SA_LOG_PERSIST - 1));
        s_store.count--;
    }
    sa_log_persisted_t *p = &s_store.e[s_store.count++];
    memset(p, 0, sizeof(*p));
    p->boot = s_boot;
    p->uptime_s = (uint32_t)(e->uptime_ms / 1000);
    p->epoch_s = (uint32_t)e->epoch_s;
    p->level = e->level;
    strlcpy(p->text, e->text, sizeof(p->text));
    s_dirty = true;
}

static int hook(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int ret = s_prev ? s_prev(fmt, args) : vprintf(fmt, args);

    char line[SA_LOG_TEXT + 40];
    vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    // Lines may start with a colour code: "\033[0;33mW (123) tag: ..."
    const char *p = line;
    if (p[0] == '\033') {
        const char *m = strchr(p, 'm');
        p = m ? m + 1 : p;
    }
    if ((p[0] == 'W' || p[0] == 'E') && p[1] == ' ' && p[2] == '(') {
        sa_log_entry_t e = {.uptime_ms = esp_timer_get_time() / 1000, .level = p[0]};
        struct timeval tv;
        gettimeofday(&tv, NULL);
        e.epoch_s = tv.tv_sec > 1700000000 ? tv.tv_sec : 0;   // only if SNTP set the clock
        sa_log_clean_line(e.text, sizeof(e.text), p);
        portENTER_CRITICAL(&s_lock);
        s_ring[s_total % SA_LOG_ENTRIES] = e;
        s_total++;
        persist_push(&e);
        portEXIT_CRITICAL(&s_lock);
    }
    return ret;
}

void sa_log_init(void)
{
    s_prev = esp_log_set_vprintf(hook);
}

// ---------------------------------------------------------------- flash

static void flush(void)
{
    static store_t copy;   // static: the writer task has a small stack
    portENTER_CRITICAL(&s_lock);
    bool dirty = s_dirty;
    if (dirty) {
        copy = s_store;
        s_dirty = false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!dirty) {
        return;
    }
    copy.version = STORE_VERSION;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, KEY_ENTRIES, &copy, sizeof(copy)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static void writer_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SA_LOG_FLUSH_S * 1000));
        flush();
    }
}

static void on_shutdown(void)
{
    flush();   // planned restarts (reboot, settings change, firmware update)
}

void sa_log_persist_start(void)
{
    store_t *loaded = calloc(1, sizeof(store_t));
    nvs_handle_t h;
    if (loaded && nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        uint32_t boot = 0;
        nvs_get_u32(h, KEY_BOOT, &boot);
        boot++;
        nvs_set_u32(h, KEY_BOOT, boot);
        size_t len = sizeof(*loaded);
        if (nvs_get_blob(h, KEY_ENTRIES, loaded, &len) != ESP_OK || len != sizeof(*loaded) ||
            loaded->version != STORE_VERSION || loaded->count > SA_LOG_PERSIST) {
            memset(loaded, 0, sizeof(*loaded));
        }
        nvs_commit(h);
        nvs_close(h);

        portENTER_CRITICAL(&s_lock);
        // Entries logged before this point (early boot) are already in s_store with boot 0:
        // put the stored ones in front of them and give them the right boot number.
        static store_t merged;   // static: ~2.3 KB, keep it off the main task stack
        merged = *loaded;
        for (int i = 0; i < s_store.count; i++) {
            if (merged.count == SA_LOG_PERSIST) {
                memmove(&merged.e[0], &merged.e[1], sizeof(merged.e[0]) * (SA_LOG_PERSIST - 1));
                merged.count--;
            }
            merged.e[merged.count] = s_store.e[i];
            merged.e[merged.count].boot = boot;
            merged.count++;
        }
        s_store = merged;
        s_boot = boot;
        s_dirty = s_dirty || s_store.count > loaded->count;
        portEXIT_CRITICAL(&s_lock);
    }
    free(loaded);
    s_persist_ready = true;

    const char *why = NULL;
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC: why = "a crash"; break;
    case ESP_RST_INT_WDT: why = "an interrupt watchdog timeout"; break;
    case ESP_RST_TASK_WDT: why = "a task watchdog timeout"; break;
    case ESP_RST_WDT: why = "a watchdog reset"; break;
    case ESP_RST_BROWNOUT: why = "a brownout (supply voltage too low)"; break;
    default: break;
    }
    if (why) {
        ESP_LOGW(TAG, "boot #%lu: restarted after %s", (unsigned long)s_boot, why);
    }
    esp_register_shutdown_handler(on_shutdown);
    xTaskCreate(writer_task, "log_writer", 2560, NULL, 1, NULL);
}

// ---------------------------------------------------------------- readers

int sa_log_get(sa_log_entry_t *out, int max)
{
    int n = 0;
    portENTER_CRITICAL(&s_lock);
    uint32_t avail = s_total < SA_LOG_ENTRIES ? s_total : SA_LOG_ENTRIES;
    for (uint32_t i = 0; i < avail && n < max; i++) {
        out[n++] = s_ring[(s_total - 1 - i) % SA_LOG_ENTRIES];
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

int sa_log_get_previous(sa_log_persisted_t *out, int max)
{
    int n = 0;
    portENTER_CRITICAL(&s_lock);
    for (int i = s_store.count - 1; i >= 0 && n < max; i--) {
        if (s_store.e[i].boot != s_boot) {
            out[n++] = s_store.e[i];
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

uint32_t sa_log_count(void)
{
    return s_total;
}

uint32_t sa_log_boot(void)
{
    return s_boot;
}

void sa_log_clear_persisted(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_store, 0, sizeof(s_store));
    s_dirty = true;
    portEXIT_CRITICAL(&s_lock);
    flush();
    ESP_LOGI(TAG, "stored log cleared");
}
