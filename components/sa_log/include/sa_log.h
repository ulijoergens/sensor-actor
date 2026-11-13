// Error log for the Status page (docs/FSD.md REQ-LOG-1, RAM part).
//
// Hooks into ESP-IDF logging and keeps the last SA_LOG_ENTRIES warnings and errors in a
// ring buffer. Persisting the last 20 to flash follows later.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SA_LOG_ENTRIES  100
#define SA_LOG_TEXT     120

typedef struct {
    int64_t uptime_ms;     // time since boot
    int64_t epoch_s;       // wall-clock time if known (SNTP), else 0
    char level;            // 'W' or 'E'
    char text[SA_LOG_TEXT];
} sa_log_entry_t;

// Installs the log hook. Call early in app_main().
void sa_log_init(void);

// Copies up to `max` entries, newest first. Returns the number copied.
int sa_log_get(sa_log_entry_t *out, int max);

// Total warnings/errors seen since boot (also counts entries that have been overwritten).
uint32_t sa_log_count(void);

#ifdef __cplusplus
}
#endif
