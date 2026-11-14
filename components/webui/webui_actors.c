// Actor REST API:
//   GET    /api/actors             all actors
//   POST   /api/actors             {"gpio": 26} add an actor
//   GET    /api/actors/<id>        one actor
//   POST   /api/actors/<id>        change fields (see actor_from_json)
//   POST   /api/actors/<id>/set    {"state": "ON" | "OFF" | "TOGGLE"}
//   DELETE /api/actors/<id>
#include <math.h>
#include <string.h>
#include "actors.h"
#include "webui_priv.h"

static const char *action_name(rule_action_t a)
{
    return a == RULE_ACTION_ON ? "on" : a == RULE_ACTION_OFF ? "off" : "none";
}

static int action_from(const char *s)
{
    if (!strcmp(s, "on")) return RULE_ACTION_ON;
    if (!strcmp(s, "off")) return RULE_ACTION_OFF;
    if (!strcmp(s, "none")) return RULE_ACTION_NONE;
    return -1;
}

static const char *const POWER_ON[] = {"off", "on", "last"};

static cJSON *actor_json(const sa_actor_info_t *a)
{
    const sa_actor_cfg_t *c = &a->cfg;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", c->id);
    cJSON_AddStringToObject(o, "label", c->label);
    cJSON_AddNumberToObject(o, "gpio", c->gpio);
    cJSON_AddBoolToObject(o, "strapping_pin", sa_gpio_is_strapping(c->gpio));
    cJSON_AddBoolToObject(o, "active", c->active);
    cJSON_AddBoolToObject(o, "favourite", c->favourite);
    cJSON_AddBoolToObject(o, "active_low", c->active_low);
    cJSON_AddStringToObject(o, "power_on", POWER_ON[c->power_on % 3]);
    cJSON_AddStringToObject(o, "state", !c->active ? "inactive" : a->on ? "ON" : "OFF");
    cJSON_AddStringToObject(o, "source", a->source);
    if (a->level >= 0) {
        cJSON_AddNumberToObject(o, "gpio_level", a->level);
    }
    cJSON_AddNumberToObject(o, "changed_s", (double)a->changed_s);

    cJSON *r = cJSON_AddObjectToObject(o, "rule");
    cJSON_AddBoolToObject(r, "enabled", c->rule_enabled);
    cJSON_AddStringToObject(r, "sensor", c->rule_sensor);
    cJSON_AddStringToObject(r, "quantity", sa_quantity_name((sa_quantity_t)c->rule_quantity));
    cJSON_AddStringToObject(r, "source", c->rule_use_median ? "median" : "last");
    cJSON *th = cJSON_AddArrayToObject(r, "thresholds");
    for (int t = 0; t < RULE_MAX_THRESHOLDS; t++) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "enabled", c->rule.th[t].enabled);
        cJSON_AddNumberToObject(j, "value", c->rule.th[t].value);
        cJSON_AddStringToObject(j, "rising", action_name(c->rule.th[t].rising));
        cJSON_AddStringToObject(j, "falling", action_name(c->rule.th[t].falling));
        cJSON_AddItemToArray(th, j);
    }
    cJSON_AddStringToObject(r, "fault", action_name(c->rule.fault));
    cJSON_AddStringToObject(r, "text", a->rule_text);
    const char *rs = !c->rule_enabled ? "off" : a->rule_sensor_missing ? "sensor missing"
                   : a->rule_faulted ? "sensor failed" : a->rule_waiting ? "waiting for a value" : "running";
    cJSON_AddStringToObject(r, "status", rs);
    return o;
}

static esp_err_t api_actors_list(httpd_req_t *req)
{
    sa_actor_info_t *a = malloc(sizeof(sa_actor_info_t) * SA_MAX_ACTORS);
    if (!a) {
        return webui_send_error(req, "500 Internal Server Error", "Out of memory");
    }
    int n = actors_list(a, SA_MAX_ACTORS);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "actors");
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, actor_json(&a[i]));
    }
    free(a);
    return webui_send_json(req, root);
}

static esp_err_t api_actors_add(httpd_req_t *req)
{
    const char *why;
    cJSON *in = webui_read_json(req, &why);
    if (!in) {
        return webui_send_error(req, "400 Bad Request", why);
    }
    cJSON *g = cJSON_GetObjectItemCaseSensitive(in, "gpio");
    int gpio = cJSON_IsNumber(g) ? g->valueint : -1;
    cJSON_Delete(in);
    if (gpio < 0 || gpio > 39) {
        return webui_send_error(req, "400 Bad Request", "gpio: a GPIO number is required");
    }
    char id[SA_ID_MAX + 1];
    if (actors_add(gpio, id, &why) != ESP_OK) {
        return webui_send_error(req, "400 Bad Request", why ? why : "Adding failed");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "id", id);
    return webui_send_json(req, o);
}

// "/api/actors/<id>" or "/api/actors/<id>/set" -> id; *is_set tells which.
static const char *parse_uri(httpd_req_t *req, char *id, size_t len, bool *is_set)
{
    const char *p = req->uri + strlen("/api/actors/");
    size_t n = strcspn(p, "/?");
    *is_set = !strncmp(p + n, "/set", 4) && (p[n + 4] == '\0' || p[n + 4] == '?');
    if (n == 0 || n >= len || (p[n] == '/' && !*is_set)) {
        return NULL;
    }
    memcpy(id, p, n);
    id[n] = '\0';
    return id;
}

static esp_err_t api_actor_get(httpd_req_t *req)
{
    char id[16];
    bool is_set;
    sa_actor_info_t a;
    if (!parse_uri(req, id, sizeof(id), &is_set) || is_set || !actors_get(id, &a)) {
        return webui_send_error(req, "404 Not Found", "Unknown actor");
    }
    return webui_send_json(req, actor_json(&a));
}

// Applies the fields present in `in` to `c`. Returns NULL or an error message.
static const char *actor_from_json(cJSON *in, sa_actor_cfg_t *c)
{
    cJSON *v;
    if ((v = cJSON_GetObjectItemCaseSensitive(in, "id"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) > SA_ID_MAX) return "ID: 1 to 8 characters a-z, 0-9, _ or -";
        strcpy(c->id, v->valuestring);
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(in, "label"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) > SA_NAME_MAX) return "Label: 1 to 32 characters";
        strcpy(c->label, v->valuestring);
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(in, "gpio"))) {
        if (!cJSON_IsNumber(v) || v->valueint < 0 || v->valueint > 39) return "GPIO: a pin number";
        c->gpio = (int8_t)v->valueint;
    }
    const struct { const char *key; bool *dst; } flags[] = {
        {"active", &c->active}, {"favourite", &c->favourite}, {"active_low", &c->active_low},
    };
    for (size_t k = 0; k < sizeof(flags) / sizeof(flags[0]); k++) {
        if ((v = cJSON_GetObjectItemCaseSensitive(in, flags[k].key))) {
            if (!cJSON_IsBool(v)) return "Flags must be true or false";
            *flags[k].dst = cJSON_IsTrue(v);
        }
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(in, "power_on"))) {
        int p = -1;
        for (int k = 0; k < 3; k++) {
            if (cJSON_IsString(v) && !strcmp(v->valuestring, POWER_ON[k])) p = k;
        }
        if (p < 0) return "Power-on state: off, on or last";
        c->power_on = (uint8_t)p;
    }
    cJSON *r = cJSON_GetObjectItemCaseSensitive(in, "rule");
    if (!r) {
        return NULL;
    }
    if (!cJSON_IsObject(r)) return "Rule: invalid";
    if ((v = cJSON_GetObjectItemCaseSensitive(r, "enabled"))) {
        if (!cJSON_IsBool(v)) return "Rule: enabled must be true or false";
        c->rule_enabled = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(r, "sensor"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) > SA_ID_MAX) return "Rule: choose a sensor";
        strcpy(c->rule_sensor, v->valuestring);
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(r, "quantity"))) {
        int q = -1;
        for (int k = 0; k < SA_Q_COUNT; k++) {
            if (cJSON_IsString(v) && !strcmp(v->valuestring, sa_quantity_name(k))) q = k;
        }
        if (q < 0) return "Rule: choose a value";
        c->rule_quantity = (uint8_t)q;
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(r, "source"))) {
        if (!cJSON_IsString(v) || (strcmp(v->valuestring, "median") && strcmp(v->valuestring, "last")))
            return "Rule: value source is median or last";
        c->rule_use_median = !strcmp(v->valuestring, "median");
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(r, "fault"))) {
        int a = cJSON_IsString(v) ? action_from(v->valuestring) : -1;
        if (a < 0) return "Rule: fault action is on, off or none";
        c->rule.fault = (rule_action_t)a;
    }
    cJSON *th = cJSON_GetObjectItemCaseSensitive(r, "thresholds");
    if (th) {
        if (!cJSON_IsArray(th) || cJSON_GetArraySize(th) > RULE_MAX_THRESHOLDS) return "Rule: up to two thresholds";
        for (int t = 0; t < cJSON_GetArraySize(th); t++) {
            cJSON *j = cJSON_GetArrayItem(th, t);
            rule_threshold_t *d = &c->rule.th[t];
            if ((v = cJSON_GetObjectItemCaseSensitive(j, "enabled"))) {
                if (!cJSON_IsBool(v)) return "Rule: threshold enabled must be true or false";
                d->enabled = cJSON_IsTrue(v);
            }
            if ((v = cJSON_GetObjectItemCaseSensitive(j, "value"))) {
                if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) return "Rule: thresholds must be numbers";
                d->value = (float)v->valuedouble;
            }
            const char *keys[2] = {"rising", "falling"};
            rule_action_t *dst[2] = {&d->rising, &d->falling};
            for (int k = 0; k < 2; k++) {
                if ((v = cJSON_GetObjectItemCaseSensitive(j, keys[k]))) {
                    int a = cJSON_IsString(v) ? action_from(v->valuestring) : -1;
                    if (a < 0) return "Rule: actions are on, off or none";
                    *dst[k] = (rule_action_t)a;
                }
            }
        }
    }
    return NULL;
}

static esp_err_t api_actor_post(httpd_req_t *req)
{
    char id[16];
    bool is_set;
    sa_actor_info_t a;
    if (!parse_uri(req, id, sizeof(id), &is_set) || !actors_get(id, &a)) {
        return webui_send_error(req, "404 Not Found", "Unknown actor");
    }
    const char *why;
    cJSON *in = webui_read_json(req, &why);
    if (!in) {
        return webui_send_error(req, "400 Bad Request", why);
    }
    if (is_set) {
        cJSON *s = cJSON_GetObjectItemCaseSensitive(in, "state");
        int cmd = cJSON_IsString(s) ? sa_actor_parse_command(s->valuestring) : -1;
        cJSON_Delete(in);
        if (cmd < 0) {
            return webui_send_error(req, "400 Bad Request", "state: ON, OFF or TOGGLE");
        }
        if (actors_command(id, cmd, "web", &why) != ESP_OK) {
            return webui_send_error(req, "400 Bad Request", why ? why : "Command failed");
        }
    } else {
        sa_actor_cfg_t c = a.cfg;
        const char *bad = actor_from_json(in, &c);
        cJSON_Delete(in);
        if (bad) {
            return webui_send_error(req, "400 Bad Request", bad);
        }
        if (actors_update(id, &c, &why) != ESP_OK) {
            return webui_send_error(req, "400 Bad Request", why ? why : "Saving failed");
        }
        strlcpy(id, c.id, sizeof(id));
    }
    actors_get(id, &a);
    return webui_send_json(req, actor_json(&a));
}

static esp_err_t api_actor_delete(httpd_req_t *req)
{
    char id[16];
    bool is_set;
    if (!parse_uri(req, id, sizeof(id), &is_set) || is_set || actors_remove(id) != ESP_OK) {
        return webui_send_error(req, "404 Not Found", "Unknown actor");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return webui_send_json(req, o);
}

void webui_register_actors(httpd_handle_t srv)
{
    const httpd_uri_t uris[] = {
        {.uri = "/api/actors", .method = HTTP_GET, .handler = api_actors_list},
        {.uri = "/api/actors", .method = HTTP_POST, .handler = api_actors_add},
        {.uri = "/api/actors/*", .method = HTTP_GET, .handler = api_actor_get},
        {.uri = "/api/actors/*", .method = HTTP_POST, .handler = api_actor_post},
        {.uri = "/api/actors/*", .method = HTTP_DELETE, .handler = api_actor_delete},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(srv, &uris[i]));
    }
}
