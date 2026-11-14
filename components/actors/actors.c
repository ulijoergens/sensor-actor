// Actors: GPIO relays, power-on state, rules and MQTT. See actors.h.
#include "actors.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sa_mqtt.h"

static const char *TAG = "actors";

#define NVS_NS        "sa"
#define KEY_ACTORS    "actors"
#define STORE_VERSION 1

typedef struct {
    uint16_t version;
    uint16_t size;
    sa_actor_cfg_t cfg[SA_MAX_ACTORS];
} store_t;

typedef struct {
    bool on;
    char source[8];
    int64_t changed_us;
    rule_state_t rs;
    bool faulted;
} runtime_t;

typedef enum { MSG_VALUE, MSG_FAULT, MSG_COMMAND, MSG_ANNOUNCE } msg_type_t;

typedef struct {
    msg_type_t type;
    char id[SA_ID_MAX + 1];      // sensor ID (value/fault) or actor ID (command)
    uint8_t quantity;
    int8_t cmd;
    float value;
    float median;
} msg_t;

static sa_actor_cfg_t s_cfg[SA_MAX_ACTORS];
static runtime_t s_rt[SA_MAX_ACTORS];
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_queue;

static void lock(void) { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

// ---------------------------------------------------------------- storage

static esp_err_t save(void)
{
    store_t *st = calloc(1, sizeof(store_t));
    if (!st) {
        return ESP_ERR_NO_MEM;
    }
    st->version = STORE_VERSION;
    st->size = sizeof(sa_actor_cfg_t);
    memcpy(st->cfg, s_cfg, sizeof(s_cfg));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, KEY_ACTORS, st, sizeof(*st));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(st);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving actor settings failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void load(void)
{
    memset(s_cfg, 0, sizeof(s_cfg));
    store_t *st = calloc(1, sizeof(store_t));
    if (!st) {
        return;
    }
    nvs_handle_t h;
    size_t len = sizeof(*st);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_blob(h, KEY_ACTORS, st, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(*st) && st->version == STORE_VERSION &&
            st->size == sizeof(sa_actor_cfg_t)) {
            for (int i = 0; i < SA_MAX_ACTORS; i++) {
                if (st->cfg[i].used &&
                    sa_actor_validate(&st->cfg[i], sa_config_get(), st->cfg, SA_MAX_ACTORS, i) == NULL) {
                    s_cfg[i] = st->cfg[i];
                } else if (st->cfg[i].used) {
                    ESP_LOGW(TAG, "stored actor \"%s\" is invalid (pins changed?), ignored", st->cfg[i].id);
                }
            }
        } else if (err == ESP_OK) {
            ESP_LOGW(TAG, "stored actor settings are from another version, ignored");
        }
    }
    free(st);
}

static int find(const char *id)
{
    for (int i = 0; i < SA_MAX_ACTORS; i++) {
        if (s_cfg[i].used && strcmp(s_cfg[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------- GPIO

static void gpio_release(int gpio)
{
    // REQ-ACT-2: inactive actors leave their pin as a floating input (high-Z).
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static void gpio_drive(int gpio, int level)
{
    gpio_set_level(gpio, level);   // set the level first: no glitch when the output turns on
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT,   // input too, so the pad level can be read back
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(gpio, level);
}

// ---------------------------------------------------------------- MQTT

static void publish_state(int i)
{
    char sub[32];
    snprintf(sub, sizeof(sub), "actor/%s/state", s_cfg[i].id);
    sa_mqtt_publish(sub, s_rt[i].on ? "ON" : "OFF", true);
}

static void announce(int i)
{
    const sa_actor_cfg_t *c = &s_cfg[i];
    char topic[128], uid[48], st[32], cmd[32];
    if (!sa_mqtt_discovery_topic(topic, sizeof(topic), "switch", c->id, uid, sizeof(uid))) {
        return;
    }
    if (!sa_config_get()->discovery || !c->active) {
        sa_mqtt_publish_topic(topic, "", true);
        return;
    }
    snprintf(st, sizeof(st), "actor/%s/state", c->id);
    snprintf(cmd, sizeof(cmd), "actor/%s/set", c->id);
    char *txt = sa_mqtt_discovery_json(c->label, uid, st, cmd, NULL, NULL, "switch", NULL, false);
    if (txt) {
        sa_mqtt_publish_topic(topic, txt, true);
        free(txt);
    }
}

static void unannounce(const sa_actor_cfg_t *c)
{
    char topic[128], uid[48], sub[32];
    if (sa_mqtt_discovery_topic(topic, sizeof(topic), "switch", c->id, uid, sizeof(uid))) {
        sa_mqtt_publish_topic(topic, "", true);
    }
    snprintf(sub, sizeof(sub), "actor/%s/state", c->id);
    sa_mqtt_publish(sub, "", true);
}

// ---------------------------------------------------------------- state changes

static void set_state(int i, bool on, const char *source)
{
    sa_actor_cfg_t *c = &s_cfg[i];
    runtime_t *r = &s_rt[i];
    if (!c->active) {
        return;
    }
    bool changed = r->on != on;
    r->on = on;
    strlcpy(r->source, source, sizeof(r->source));
    gpio_drive(c->gpio, sa_actor_gpio_level(on, c->active_low));
    if (changed) {
        r->changed_us = esp_timer_get_time();
        ESP_LOGI(TAG, "%s: %s (%s)", c->id, on ? "ON" : "OFF", source);
        if (c->power_on == SA_POWER_ON_LAST && c->last_on != on) {
            c->last_on = on;
            save();
        }
    }
    publish_state(i);
}

static void setup_actor(int i, bool at_boot)
{
    sa_actor_cfg_t *c = &s_cfg[i];
    runtime_t *r = &s_rt[i];
    memset(r, 0, sizeof(*r));
    rule_state_reset(&r->rs);
    if (!c->active) {
        gpio_release(c->gpio);
        strlcpy(r->source, "inactive", sizeof(r->source));
        return;
    }
    bool on = c->power_on == SA_POWER_ON_ON || (c->power_on == SA_POWER_ON_LAST && c->last_on);
    r->on = !on;   // force set_state to treat it as a change
    set_state(i, on, at_boot ? "boot" : "config");
}

static const char *cmd_source_ok(int i)
{
    return s_cfg[i].active ? NULL : "Actor is inactive";
}

static void apply_command(int i, int cmd, const char *source)
{
    bool on = cmd == 2 ? !s_rt[i].on : cmd == 1;
    set_state(i, on, source);
}

static void on_sensor_msg(const msg_t *m)
{
    for (int i = 0; i < SA_MAX_ACTORS; i++) {
        sa_actor_cfg_t *c = &s_cfg[i];
        runtime_t *r = &s_rt[i];
        if (!c->used || !c->active || !c->rule_enabled || strcmp(c->rule_sensor, m->id) != 0 ||
            c->rule_quantity != m->quantity) {
            continue;
        }
        if (m->type == MSG_FAULT) {
            rule_action_t a = rule_fault(&c->rule, &r->rs);
            if (!r->faulted) {
                ESP_LOGW(TAG, "%s: rule sensor %s failed%s", c->id, c->rule_sensor,
                         a == RULE_ACTION_NONE ? ", keeping the current state" : "");
            }
            r->faulted = true;
            if (a != RULE_ACTION_NONE) {
                set_state(i, a == RULE_ACTION_ON, "fault");
            }
            continue;
        }
        r->faulted = false;
        float v = c->rule_use_median ? m->median : m->value;
        if (!isfinite(v)) {
            continue;
        }
        rule_action_t a = rule_update(&c->rule, &r->rs, v);
        if (a != RULE_ACTION_NONE) {
            set_state(i, a == RULE_ACTION_ON, "rule");
        }
    }
}

static void task(void *arg)
{
    msg_t m;
    for (;;) {
        if (xQueueReceive(s_queue, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        lock();
        switch (m.type) {
        case MSG_VALUE:
        case MSG_FAULT:
            on_sensor_msg(&m);
            break;
        case MSG_COMMAND: {
            int i = find(m.id);
            if (i < 0 || cmd_source_ok(i)) {
                ESP_LOGW(TAG, "MQTT command for unknown or inactive actor \"%s\" ignored", m.id);
            } else {
                apply_command(i, m.cmd, "mqtt");
            }
            break;
        }
        case MSG_ANNOUNCE:
            for (int i = 0; i < SA_MAX_ACTORS; i++) {
                if (s_cfg[i].used) {
                    announce(i);
                    if (s_cfg[i].active) {
                        publish_state(i);
                    }
                }
            }
            break;
        }
        unlock();
    }
}

// ---------------------------------------------------------------- callbacks (other tasks)

static void on_sensor_value(const char *sensor_id, sa_quantity_t q, float value, float median, bool fault)
{
    msg_t m = {.type = fault ? MSG_FAULT : MSG_VALUE, .quantity = q, .value = value, .median = median};
    strlcpy(m.id, sensor_id, sizeof(m.id));
    xQueueSend(s_queue, &m, 0);   // never block the sensor task
}

static bool on_mqtt_command(const char *aid, const char *payload)
{
    int cmd = sa_actor_parse_command(payload);
    if (cmd < 0) {
        ESP_LOGW(TAG, "invalid command \"%s\" for actor \"%s\" (ON, OFF or TOGGLE)", payload, aid);
        return true;   // already logged
    }
    msg_t m = {.type = MSG_COMMAND, .cmd = (int8_t)cmd};
    strlcpy(m.id, aid, sizeof(m.id));
    xQueueSend(s_queue, &m, 0);
    return true;       // unknown actors are reported by the actor task
}

static void on_mqtt_connect(void)
{
    msg_t m = {.type = MSG_ANNOUNCE};
    xQueueSend(s_queue, &m, 0);
}

// ---------------------------------------------------------------- public API

esp_err_t actors_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    s_queue = xQueueCreate(32, sizeof(msg_t));
    load();
    int n = 0;
    lock();
    for (int i = 0; i < SA_MAX_ACTORS; i++) {
        if (s_cfg[i].used) {
            setup_actor(i, true);
            n++;
        }
    }
    unlock();
    ESP_LOGI(TAG, "%d actor(s) configured", n);
    sensors_set_value_callback(on_sensor_value);
    sa_mqtt_set_command_handler(on_mqtt_command);
    sa_mqtt_add_connect_handler(on_mqtt_connect);
    return xTaskCreate(task, "actors", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}

static void fill_info(int i, sa_actor_info_t *o, int64_t now)
{
    const runtime_t *r = &s_rt[i];
    memset(o, 0, sizeof(*o));
    o->cfg = s_cfg[i];
    o->on = s_cfg[i].active && r->on;
    o->level = s_cfg[i].active ? gpio_get_level(s_cfg[i].gpio) : -1;
    strlcpy(o->source, r->source, sizeof(o->source));
    o->changed_s = r->changed_us ? (now - r->changed_us) / 1000000 : now / 1000000;
    o->rule_faulted = r->faulted;
    o->rule_waiting = s_cfg[i].rule_enabled && !r->rs.has_prev && !r->faulted;
}

// Rule text and sensor check need the sensor module; done outside the actor lock.
static void finish_info(sa_actor_info_t *o)
{
    const sa_actor_cfg_t *c = &o->cfg;
    if (!c->rule_enabled) {
        strlcpy(o->rule_text, "No rule", sizeof(o->rule_text));
        return;
    }
    sa_sensor_info_t s;
    bool found = false;
    if (sensors_get(c->rule_sensor, &s)) {
        for (int k = 0; k < s.nq; k++) {
            found |= s.q[k] == c->rule_quantity;
        }
    }
    o->rule_sensor_missing = !found;
    char q[40];
    snprintf(q, sizeof(q), "%s %s", c->rule_sensor, sa_quantity_name((sa_quantity_t)c->rule_quantity));
    rule_describe(&c->rule, q, o->rule_text, sizeof(o->rule_text));
}

int actors_list(sa_actor_info_t *out, int max)
{
    int n = 0;
    int64_t now = esp_timer_get_time();
    lock();
    for (int i = 0; i < SA_MAX_ACTORS && n < max; i++) {
        if (s_cfg[i].used) {
            fill_info(i, &out[n++], now);
        }
    }
    unlock();
    for (int k = 0; k < n; k++) {
        finish_info(&out[k]);
    }
    return n;
}

bool actors_get(const char *id, sa_actor_info_t *out)
{
    lock();
    int i = find(id);
    if (i >= 0) {
        fill_info(i, out, esp_timer_get_time());
    }
    unlock();
    if (i >= 0) {
        finish_info(out);
    }
    return i >= 0;
}

esp_err_t actors_add(int gpio, char id_out[SA_ID_MAX + 1], const char **why)
{
    *why = NULL;
    lock();
    int slot = -1;
    for (int i = 0; i < SA_MAX_ACTORS && slot < 0; i++) {
        if (!s_cfg[i].used) {
            slot = i;
        }
    }
    if (slot < 0) {
        unlock();
        *why = "All 8 actor slots are in use";
        return ESP_ERR_NO_MEM;
    }
    sa_actor_cfg_t c;
    for (int n = 1; n < 100; n++) {
        sa_actor_defaults(&c, gpio, n);
        if (find(c.id) < 0) {
            break;
        }
    }
    *why = sa_actor_validate(&c, sa_config_get(), s_cfg, SA_MAX_ACTORS, slot);
    if (*why) {
        unlock();
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg[slot] = c;
    esp_err_t err = save();
    if (err == ESP_OK) {
        setup_actor(slot, false);
        announce(slot);
        strlcpy(id_out, c.id, SA_ID_MAX + 1);
        ESP_LOGI(TAG, "added %s on GPIO %d%s", c.id, gpio,
                 sa_gpio_is_strapping(gpio) ? " (strapping pin: may toggle during boot)" : "");
    } else {
        memset(&s_cfg[slot], 0, sizeof(s_cfg[slot]));
        *why = "Saving failed";
    }
    unlock();
    return err;
}

esp_err_t actors_update(const char *id, const sa_actor_cfg_t *in, const char **why)
{
    lock();
    int i = find(id);
    if (i < 0) {
        unlock();
        *why = "Unknown actor";
        return ESP_ERR_NOT_FOUND;
    }
    sa_actor_cfg_t c = *in;
    c.used = true;
    c.last_on = s_cfg[i].last_on;
    *why = sa_actor_validate(&c, sa_config_get(), s_cfg, SA_MAX_ACTORS, i);
    if (*why) {
        unlock();
        return ESP_ERR_INVALID_ARG;
    }
    sa_actor_cfg_t old = s_cfg[i];
    bool hw = c.gpio != old.gpio || c.active != old.active || c.active_low != old.active_low;
    bool rule = c.rule_enabled != old.rule_enabled || strcmp(c.rule_sensor, old.rule_sensor) ||
                c.rule_quantity != old.rule_quantity || c.rule_use_median != old.rule_use_median ||
                memcmp(&c.rule, &old.rule, sizeof(c.rule)) != 0;
    if (strcmp(c.id, old.id) != 0 || !c.active) {
        unannounce(&old);
    }
    if (hw) {
        if (c.gpio != old.gpio || !c.active) {
            gpio_release(old.gpio);
        }
    }
    s_cfg[i] = c;
    esp_err_t err = save();
    if (err != ESP_OK) {
        s_cfg[i] = old;
        *why = "Saving failed";
        unlock();
        return err;
    }
    if (hw) {
        bool was_on = s_rt[i].on && old.active;
        setup_actor(i, false);
        if (c.active && old.active && c.gpio == old.gpio) {
            set_state(i, was_on, "config");   // keep the state across an active-low change
        }
    } else if (rule) {
        rule_state_reset(&s_rt[i].rs);   // the next value acts like a first sample
        s_rt[i].faulted = false;
    }
    announce(i);
    if (c.active) {
        publish_state(i);
    }
    ESP_LOGI(TAG, "%s: settings changed", c.id);
    unlock();
    return ESP_OK;
}

esp_err_t actors_remove(const char *id)
{
    lock();
    int i = find(id);
    if (i < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    sa_actor_cfg_t old = s_cfg[i];
    unannounce(&old);
    gpio_release(old.gpio);
    memset(&s_cfg[i], 0, sizeof(s_cfg[i]));
    memset(&s_rt[i], 0, sizeof(s_rt[i]));
    esp_err_t err = save();
    if (err != ESP_OK) {
        s_cfg[i] = old;
        setup_actor(i, false);
    } else {
        ESP_LOGI(TAG, "removed %s (GPIO %d released)", old.id, old.gpio);
    }
    unlock();
    return err;
}

esp_err_t actors_command(const char *id, int cmd, const char *source, const char **why)
{
    *why = NULL;
    if (cmd < 0 || cmd > 2) {
        *why = "Command: ON, OFF or TOGGLE";
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    int i = find(id);
    if (i < 0) {
        unlock();
        *why = "Unknown actor";
        return ESP_ERR_NOT_FOUND;
    }
    if ((*why = cmd_source_ok(i)) != NULL) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    apply_command(i, cmd, source);
    unlock();
    return ESP_OK;
}
