// Crash reports (docs/FSD.md REQ-LOG-2).
//
// ESP-IDF writes a core dump (ELF) to the "coredump" partition when the firmware panics,
// including task and interrupt watchdog timeouts. sa_crash reads its summary at boot,
// logs it, and offers it to the web UI.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool present;
    char task[16];
    uint32_t pc;
    uint32_t exc_cause;
    uint32_t exc_vaddr;
    char reason[120];
    uint32_t bt[16];
    uint32_t bt_depth;
    bool bt_corrupted;
    size_t size;              // dump size in bytes
    char elf_sha256[17];      // first 16 hex characters of the crashing firmware's ELF hash
} sa_crash_info_t;

// Reads the stored dump (if any) and logs a summary line the first time it is seen.
// Call once at boot, after nvs_flash_init().
void sa_crash_init(void);

// Summary of the stored dump (present = false if there is none).
void sa_crash_get(sa_crash_info_t *out);

// Reads `len` bytes of the raw dump starting at `offset`.
esp_err_t sa_crash_read(size_t offset, void *buf, size_t len);

// Erases the stored dump.
esp_err_t sa_crash_erase(void);

#ifdef __cplusplus
}
#endif
