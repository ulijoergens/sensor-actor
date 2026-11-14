// Error log for the Status page (docs/FSD.md REQ-LOG-1).
//
// Hooks into ESP-IDF logging and keeps the last SA_LOG_ENTRIES warnings and errors of the
// current boot in RAM. The last SA_LOG_PERSIST warnings/errors (of any boot) are also kept
// in NVS, so the log survives restarts and crashes. Flash writes happen in a background
// task, at most once per SA_LOG_FLUSH_S seconds and only when something new was logged,
// plus immediately before a planned restart.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SA_LOG_ENTRIES  100
#define SA_LOG_TEXT     120
#define SA_LOG_PERSIST  20
#define SA_LOG_FLUSH_S  60

typedef struct {
    int64_t uptime_ms;     // time since boot
    int64_t epoch_s;       // wall-clock time if known (SNTP), else 0
    char level;            // 'W' or 'E'
    char text[SA_LOG_TEXT];
} sa_log_entry_t;

// Stored form (flash), from any boot.
typedef struct {
    uint32_t boot;         // boot number (counts up on every start)
    uint32_t uptime_s;
    uint32_t epoch_s;      // 0 if unknown
    char level;
    char text[99];
} sa_log_persisted_t;

// Installs the log hook. Call first thing in app_main().
void sa_log_init(void);

// Loads the stored log, counts the boot, logs an abnormal restart reason and starts the
// background writer. Call after nvs_flash_init().
void sa_log_persist_start(void);

// Current boot: copies up to `max` entries, newest first. Returns the number copied.
int sa_log_get(sa_log_entry_t *out, int max);

// Stored entries from earlier boots, newest first. Returns the number copied.
int sa_log_get_previous(sa_log_persisted_t *out, int max);

// Total warnings/errors seen since boot (also counts entries that have been overwritten).
uint32_t sa_log_count(void);

// Number of this boot (0 before sa_log_persist_start()).
uint32_t sa_log_boot(void);

// Removes the "W (1234) tag: " prefix, colour codes and line ends (exposed for tests).
void sa_log_clean_line(char *dst, size_t len, const char *src);

// Erases the stored log (current-boot RAM entries are kept).
void sa_log_clear_persisted(void);

#ifdef __cplusplus
}
#endif
