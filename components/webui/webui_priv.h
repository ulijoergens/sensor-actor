// Shared helpers of the web UI handlers.
#pragma once

#include "cJSON.h"
#include "esp_http_server.h"

esp_err_t webui_send_json(httpd_req_t *req, cJSON *root);   // takes ownership of root
esp_err_t webui_send_error(httpd_req_t *req, const char *status, const char *msg);
cJSON *webui_read_json(httpd_req_t *req, const char **why);  // NULL + why on error

void webui_register_sensors(httpd_handle_t srv);
void webui_register_actors(httpd_handle_t srv);
