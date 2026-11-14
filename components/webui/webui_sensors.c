// Sensor REST API:
//   GET    /api/scan             I2C scan, marks devices that already have a sensor
//   GET    /api/sensors          all sensors with values
//   POST   /api/sensors          {"addr": 41, "chip": 8} add a scanned device
//   GET    /api/sensors/<id>     one sensor
//   POST   /api/sensors/<id>     change fields (id, label, active, favourite, interval_s,
//                                median_n, vl_mode, mount_mm, offsets{quantity: value})
//   DELETE /api/sensors/<id>
#include <math.h>
#include <string.h>
#include "sensors.h"
#include "webui_priv.h"

static cJSON *sensor_json(const sa_sensor_info_t *s)
{
    const sa_sensor_cfg_t *c = &s->cfg;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", c->id);
    cJSON_AddStringToObject(o, "label", c->label);
    cJSON_AddNumberToObject(o, "chip", c->chip);
    cJSON_AddStringToObject(o, "chip_name", sa_chip_name((sa_chip_t)c->chip));
    cJSON_AddNumberToObject(o, "addr", c->addr);
    cJSON_AddBoolToObject(o, "active", c->active);
    cJSON_AddBoolToObject(o, "favourite", c->favourite);
    cJSON_AddNumberToObject(o, "interval_s", c->interval_s);
    cJSON_AddNumberToObject(o, "median_n", c->median_n);
    if (c->chip == SA_CHIP_VL53L0X || c->chip == SA_CHIP_VL53L1X) {
        cJSON_AddStringToObject(o, "vl_mode", c->vl_mode == 1 ? "short" : "long");
        cJSON_AddNumberToObject(o, "mount_mm", c->mount_mm);
    }
    cJSON_AddStringToObject(o, "state", sa_sensor_state_name(s->state));
    cJSON_AddStringToObject(o, "error", s->error);

    cJSON *off = cJSON_AddObjectToObject(o, "offsets");
    cJSON *qs = cJSON_AddArrayToObject(o, "quantities");
    for (int k = 0; k < s->nq; k++) {
        sa_quantity_t q = s->q[k];
        cJSON_AddNumberToObject(off, sa_quantity_name(q), c->offset[q]);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", sa_quantity_name(q));
        cJSON_AddStringToObject(j, "unit", sa_quantity_unit(q));
        cJSON_AddBoolToObject(j, "valid", s->v[k].valid);
        cJSON_AddBoolToObject(j, "stale", s->v[k].stale);
        if (s->v[k].valid) {
            cJSON_AddNumberToObject(j, "value", roundf(s->v[k].value * 10) / 10);
            cJSON_AddNumberToObject(j, "median", roundf(s->v[k].median * 10) / 10);
            cJSON_AddNumberToObject(j, "age_s", (double)(s->v[k].age_ms / 1000));
        }
        cJSON_AddItemToArray(qs, j);
    }
    return o;
}

static esp_err_t api_scan(httpd_req_t *req)
{
    sa_scan_entry_t e[16];
    int n = sensors_scan(e, 16);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "devices");
    for (int i = 0; i < n; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "addr", e[i].dev.addr);
        cJSON_AddNumberToObject(d, "chip", e[i].dev.chip);
        cJSON_AddStringToObject(d, "chip_name", sa_chip_name(e[i].dev.chip));
        cJSON_AddBoolToObject(d, "supported", e[i].dev.supported);
        cJSON_AddStringToObject(d, "detail", e[i].dev.detail);
        cJSON_AddBoolToObject(d, "configured", e[i].configured);
        cJSON_AddStringToObject(d, "sensor_id", e[i].sensor_id);
        cJSON_AddItemToArray(arr, d);
    }
    return webui_send_json(req, root);
}

static esp_err_t api_sensors_list(httpd_req_t *req)
{
    sa_sensor_info_t *s = malloc(sizeof(sa_sensor_info_t) * SA_MAX_SENSORS);
    if (!s) {
        return webui_send_error(req, "500 Internal Server Error", "Out of memory");
    }
    int n = sensors_list(s, SA_MAX_SENSORS);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "sensors");
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, sensor_json(&s[i]));
    }
    free(s);
    return webui_send_json(req, root);
}

static esp_err_t api_sensors_add(httpd_req_t *req)
{
    const char *why;
    cJSON *in = webui_read_json(req, &why);
    if (!in) {
        return webui_send_error(req, "400 Bad Request", why);
    }
    cJSON *a = cJSON_GetObjectItemCaseSensitive(in, "addr");
    cJSON *c = cJSON_GetObjectItemCaseSensitive(in, "chip");
    if (!cJSON_IsNumber(a) || !cJSON_IsNumber(c) || a->valueint < 0x08 || a->valueint > 0x77) {
        cJSON_Delete(in);
        return webui_send_error(req, "400 Bad Request", "addr (0x08-0x77) and chip are required");
    }
    uint8_t addr = (uint8_t)a->valueint, chip = (uint8_t)c->valueint;
    cJSON_Delete(in);

    // Only add what is actually on the bus, as identified by the scan.
    sa_scan_entry_t e[16];
    int n = sensors_scan(e, 16);
    bool present = false;
    for (int i = 0; i < n; i++) {
        if (e[i].dev.addr == addr && e[i].dev.chip == chip && e[i].dev.supported) {
            present = true;
        }
    }
    if (!present) {
        return webui_send_error(req, "400 Bad Request", "No supported sensor of that type at this address");
    }
    char id[SA_ID_MAX + 1];
    if (sensors_add(chip, addr, id, &why) != ESP_OK) {
        return webui_send_error(req, "400 Bad Request", why ? why : "Adding failed");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "id", id);
    return webui_send_json(req, o);
}

static const char *id_from_uri(httpd_req_t *req, char *id, size_t len)
{
    const char *p = req->uri + strlen("/api/sensors/");
    size_t n = strcspn(p, "?");
    if (n == 0 || n >= len) {
        return NULL;
    }
    memcpy(id, p, n);
    id[n] = '\0';
    return id;
}

static esp_err_t api_sensor_get(httpd_req_t *req)
{
    char id[16];
    sa_sensor_info_t s;
    if (!id_from_uri(req, id, sizeof(id)) || !sensors_get(id, &s)) {
        return webui_send_error(req, "404 Not Found", "Unknown sensor");
    }
    return webui_send_json(req, sensor_json(&s));
}

static const char *take_num(cJSON *in, const char *key, double min, double max, double *out, const char *msg)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(in, key);
    if (!v) {
        return NULL;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max) {
        return msg;
    }
    *out = v->valuedouble;
    return NULL;
}

static esp_err_t api_sensor_post(httpd_req_t *req)
{
    char id[16];
    sa_sensor_info_t s;
    if (!id_from_uri(req, id, sizeof(id)) || !sensors_get(id, &s)) {
        return webui_send_error(req, "404 Not Found", "Unknown sensor");
    }
    const char *why;
    cJSON *in = webui_read_json(req, &why);
    if (!in) {
        return webui_send_error(req, "400 Bad Request", why);
    }
    sa_sensor_cfg_t c = s.cfg;
    const char *bad = NULL;
    cJSON *v;
    if ((v = cJSON_GetObjectItemCaseSensitive(in, "id"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) > SA_ID_MAX) bad = "ID: 1 to 8 characters a-z, 0-9, _ or -";
        else strcpy(c.id, v->valuestring);
    }
    if (!bad && (v = cJSON_GetObjectItemCaseSensitive(in, "label"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) > SA_NAME_MAX) bad = "Label: 1 to 32 characters";
        else strcpy(c.label, v->valuestring);
    }
    if (!bad && (v = cJSON_GetObjectItemCaseSensitive(in, "active"))) {
        if (!cJSON_IsBool(v)) bad = "active: true or false"; else c.active = cJSON_IsTrue(v);
    }
    if (!bad && (v = cJSON_GetObjectItemCaseSensitive(in, "favourite"))) {
        if (!cJSON_IsBool(v)) bad = "favourite: true or false"; else c.favourite = cJSON_IsTrue(v);
    }
    double interval = c.interval_s, median = c.median_n, mount = c.mount_mm;
    if (!bad) bad = take_num(in, "interval_s", 1, 3600, &interval, "Read interval: 1 to 3600 s");
    if (!bad) bad = take_num(in, "median_n", 3, SA_MEDIAN_MAX, &median, "Median window: 3 to 31 samples");
    if (!bad) bad = take_num(in, "mount_mm", 0, 4000, &mount, "Mounting height: 0 (off) to 4000 mm");
    c.interval_s = (uint16_t)interval;
    c.median_n = (uint8_t)median;
    c.mount_mm = (uint16_t)mount;
    if (!bad && (v = cJSON_GetObjectItemCaseSensitive(in, "vl_mode"))) {
        if (cJSON_IsString(v) && !strcmp(v->valuestring, "short")) c.vl_mode = 1;
        else if (cJSON_IsString(v) && !strcmp(v->valuestring, "long")) c.vl_mode = 2;
        else bad = "Distance mode: short or long";
    }
    cJSON *off = cJSON_GetObjectItemCaseSensitive(in, "offsets");
    if (!bad && off) {
        cJSON *it;
        cJSON_ArrayForEach(it, off) {
            int q = -1;
            for (int k = 0; k < SA_Q_COUNT; k++) {
                if (!strcmp(it->string, sa_quantity_name(k))) q = k;
            }
            if (q < 0 || !cJSON_IsNumber(it) || fabs(it->valuedouble) > 10000) {
                bad = "Offset: a number between -10000 and 10000";
                break;
            }
            c.offset[q] = (float)it->valuedouble;
        }
    }
    cJSON_Delete(in);
    if (bad) {
        return webui_send_error(req, "400 Bad Request", bad);
    }
    if (sensors_update(id, &c, &why) != ESP_OK) {
        return webui_send_error(req, "400 Bad Request", why ? why : "Saving failed");
    }
    sensors_get(c.id, &s);
    return webui_send_json(req, sensor_json(&s));
}

static esp_err_t api_sensor_delete(httpd_req_t *req)
{
    char id[16];
    if (!id_from_uri(req, id, sizeof(id)) || sensors_remove(id) != ESP_OK) {
        return webui_send_error(req, "404 Not Found", "Unknown sensor");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return webui_send_json(req, o);
}

void webui_register_sensors(httpd_handle_t srv)
{
    const httpd_uri_t uris[] = {
        {.uri = "/api/scan", .method = HTTP_GET, .handler = api_scan},
        {.uri = "/api/sensors", .method = HTTP_GET, .handler = api_sensors_list},
        {.uri = "/api/sensors", .method = HTTP_POST, .handler = api_sensors_add},
        {.uri = "/api/sensors/*", .method = HTTP_GET, .handler = api_sensor_get},
        {.uri = "/api/sensors/*", .method = HTTP_POST, .handler = api_sensor_post},
        {.uri = "/api/sensors/*", .method = HTTP_DELETE, .handler = api_sensor_delete},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        webui_register(srv, &uris[i]);
    }
}
