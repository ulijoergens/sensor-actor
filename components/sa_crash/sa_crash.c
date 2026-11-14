// Crash reports from the core dump partition, see sa_crash.h.
#include "sa_crash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_log.h"

static const char *TAG = "crash";

static sa_crash_info_t s_info;
static size_t s_addr;

static void load(void)
{
    memset(&s_info, 0, sizeof(s_info));
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &size) != ESP_OK) {
        return;
    }
    s_addr = addr;
    s_info.present = true;
    s_info.size = size;

    esp_core_dump_summary_t *sum = calloc(1, sizeof(*sum));
    if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
        strlcpy(s_info.task, sum->exc_task, sizeof(s_info.task));
        s_info.pc = sum->exc_pc;
        s_info.exc_cause = sum->ex_info.exc_cause;
        s_info.exc_vaddr = sum->ex_info.exc_vaddr;
        s_info.bt_depth = sum->exc_bt_info.depth > 16 ? 16 : sum->exc_bt_info.depth;
        memcpy(s_info.bt, sum->exc_bt_info.bt, sizeof(uint32_t) * s_info.bt_depth);
        s_info.bt_corrupted = sum->exc_bt_info.corrupted;
        for (int i = 0; i < 8; i++) {
            snprintf(s_info.elf_sha256 + 2 * i, 3, "%02x", sum->app_elf_sha256[i]);
        }
    }
    free(sum);
    if (esp_core_dump_get_panic_reason(s_info.reason, sizeof(s_info.reason)) != ESP_OK) {
        s_info.reason[0] = '\0';
    }
}

void sa_crash_init(void)
{
    load();
    if (!s_info.present) {
        return;
    }
    char bt[16 * 11 + 1] = "";
    for (uint32_t i = 0; i < s_info.bt_depth && i < 6; i++) {
        snprintf(bt + strlen(bt), sizeof(bt) - strlen(bt), " 0x%08lx", (unsigned long)s_info.bt[i]);
    }
    ESP_LOGE(TAG, "crash report stored: task %s, PC 0x%08lx%s%s, backtrace%s", s_info.task[0] ? s_info.task : "?",
             (unsigned long)s_info.pc, s_info.reason[0] ? ", " : "", s_info.reason, bt);
}

void sa_crash_get(sa_crash_info_t *out)
{
    *out = s_info;
}

esp_err_t sa_crash_read(size_t offset, void *buf, size_t len)
{
    if (!s_info.present || offset + len > s_info.size) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_flash_read(NULL, buf, s_addr + offset, len);
}

esp_err_t sa_crash_erase(void)
{
    esp_err_t err = esp_core_dump_image_erase();
    if (err == ESP_OK) {
        memset(&s_info, 0, sizeof(s_info));
        ESP_LOGI(TAG, "crash report erased");
    }
    return err;
}
