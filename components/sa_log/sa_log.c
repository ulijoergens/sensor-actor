// Ring buffer of warnings and errors, fed by a vprintf hook on ESP-IDF logging.
#include "sa_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static sa_log_entry_t s_ring[SA_LOG_ENTRIES];
static uint32_t s_total;       // entries ever written; next slot = s_total % SA_LOG_ENTRIES
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;

// Strip ANSI colour codes, the "W (12345) " prefix and the trailing newline.
static void clean(char *dst, size_t len, const char *src)
{
    const char *p = src;
    if ((p[0] == 'W' || p[0] == 'E') && p[1] == ' ' && p[2] == '(') {
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
        clean(e.text, sizeof(e.text), p);
        portENTER_CRITICAL(&s_lock);
        s_ring[s_total % SA_LOG_ENTRIES] = e;
        s_total++;
        portEXIT_CRITICAL(&s_lock);
    }
    return ret;
}

void sa_log_init(void)
{
    s_prev = esp_log_set_vprintf(hook);
}

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

uint32_t sa_log_count(void)
{
    return s_total;
}
