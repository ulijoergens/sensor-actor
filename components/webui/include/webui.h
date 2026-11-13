// Built-in web UI and JSON REST API (docs/FSD.md §7).
//
//   GET  /                 single-page UI (index.html, app.js, style.css)
//   GET  /api/status       device, WiFi and log summary
//   GET  /api/settings     settings; passwords are never returned (only *_set flags)
//   POST /api/settings     JSON with the fields to change; validated, saved, then reboot
//   GET  /api/log          warnings and errors, newest first
//   POST /api/reboot
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t webui_start(void);

#ifdef __cplusplus
}
#endif
