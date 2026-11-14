// Sensor framework: storage, read task, values, MQTT. See sensors.h.
#include "sensors.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "drv_vl53l1x.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sa_mqtt.h"

static const char *TAG = "sensors";

#define NVS_NS         "sa"
#define KEY_SENSORS    "sensors"
#define STORE_VERSION  1
#define FAILS_TO_ERROR 3            // REQ-SEN-5
#define RETRY_INIT_US  (30LL * 1000 * 1000)
#define RETRY_ERROR_US (10LL * 1000 * 1000)   // re-init interval while a sensor is in error

typedef struct {
    uint16_t version;
    uint16_t size;
    sa_sensor_cfg_t cfg[SA_MAX_SENSORS];
} store_t;

typedef struct {
    sa_sensor_state_t state;
    char error[48];
    int fails;
    int64_t next_due_us;
    bool inited;
    vl53l1x_t vl;
    uint8_t nq;
    sa_quantity_t q[SA_MAX_QUANTITIES];
    float hist[SA_MAX_QUANTITIES][SA_MEDIAN_MAX];
    uint8_t hist_n[SA_MAX_QUANTITIES];
    uint8_t hist_pos[SA_MAX_QUANTITIES];
    float last[SA_MAX_QUANTITIES];
    int64_t last_us[SA_MAX_QUANTITIES];
    bool valid[SA_MAX_QUANTITIES];
} runtime_t;

static sa_sensor_cfg_t s_cfg[SA_MAX_SENSORS];
static runtime_t s_rt[SA_MAX_SENSORS];
static SemaphoreHandle_t s_mutex;
static volatile bool s_announce_pending;
static sa_sensor_value_cb_t s_value_cb;

void sensors_set_value_callback(sa_sensor_value_cb_t cb)
{
    s_value_cb = cb;
}

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
    st->size = sizeof(sa_sensor_cfg_t);
    memcpy(st->cfg, s_cfg, sizeof(s_cfg));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, KEY_SENSORS, st, sizeof(*st));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(st);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving sensor settings failed: %s", esp_err_to_name(err));
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
        esp_err_t err = nvs_get_blob(h, KEY_SENSORS, st, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(*st) && st->version == STORE_VERSION &&
            st->size == sizeof(sa_sensor_cfg_t)) {
            for (int i = 0; i < SA_MAX_SENSORS; i++) {
                if (st->cfg[i].used && sa_sensor_validate(&st->cfg[i]) == NULL) {
                    s_cfg[i] = st->cfg[i];
                }
            }
        } else if (err == ESP_OK) {
            ESP_LOGW(TAG, "stored sensor settings are from another version, ignored");
        }
    }
    free(st);
}

// ---------------------------------------------------------------- runtime helpers

static void reset_runtime(int i)
{
    runtime_t *r = &s_rt[i];
    if (r->inited && r->vl.dev) {
        vl53l1x_deinit(&r->vl);
    }
    memset(r, 0, sizeof(*r));
    r->nq = sa_sensor_quantities(&s_cfg[i], r->q);
    r->state = s_cfg[i].active ? SA_SENSOR_STARTING : SA_SENSOR_INACTIVE;
}

static int find(const char *id)
{
    for (int i = 0; i < SA_MAX_SENSORS; i++) {
        if (s_cfg[i].used && strcmp(s_cfg[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static const char *fmt_for(sa_quantity_t q)
{
    return (q == SA_Q_DISTANCE || q == SA_Q_LEVEL) ? "%.0f" : "%.1f";
}

// ---------------------------------------------------------------- MQTT

static void quantity_label(char *out, size_t len, const sa_sensor_cfg_t *c, sa_quantity_t q)
{
    const char *n = sa_quantity_name(q);
    snprintf(out, len, "%s %c%s", c->label, n[0] - 32, n + 1);   // "Tank Level"
}

static void announce(int i)
{
    const sa_sensor_cfg_t *c = &s_cfg[i];
    const runtime_t *r = &s_rt[i];
    const bool on = sa_config_get()->discovery && c->active;
    for (int k = 0; k < r->nq; k++) {
        sa_quantity_t q = r->q[k];
        char key[24], topic[128], uid[48], sub[48], name[48];
        snprintf(key, sizeof(key), "%s_%s", c->id, sa_quantity_name(q));
        if (!sa_mqtt_discovery_topic(topic, sizeof(topic), "sensor", key, uid, sizeof(uid))) {
            continue;
        }
        if (!on) {
            sa_mqtt_publish_topic(topic, "", true);
            continue;
        }
        snprintf(sub, sizeof(sub), "sensor/%s/%s", c->id, sa_quantity_name(q));
        quantity_label(name, sizeof(name), c, q);
        const char *dc = q == SA_Q_TEMPERATURE ? "temperature" : q == SA_Q_HUMIDITY ? "humidity"
                       : q == SA_Q_PRESSURE ? "pressure" : "distance";
        char *txt = sa_mqtt_discovery_json(name, uid, sub, NULL, NULL, sa_quantity_unit(q), dc, "measurement", false);
        if (txt) {
            sa_mqtt_publish_topic(topic, txt, true);
            free(txt);
        }
    }
}

// Remove discovery entries of a sensor configuration (before delete / rename / quantity change).
static void unannounce(const sa_sensor_cfg_t *c)
{
    sa_quantity_t q[SA_MAX_QUANTITIES];
    int nq = sa_sensor_quantities(c, q);
    for (int k = 0; k < nq; k++) {
        char key[24], topic[128], uid[48], sub[48];
        snprintf(key, sizeof(key), "%s_%s", c->id, sa_quantity_name(q[k]));
        if (sa_mqtt_discovery_topic(topic, sizeof(topic), "sensor", key, uid, sizeof(uid))) {
            sa_mqtt_publish_topic(topic, "", true);
        }
        snprintf(sub, sizeof(sub), "sensor/%s/%s", c->id, sa_quantity_name(q[k]));
        sa_mqtt_publish(sub, "", true);   // clear the retained value
    }
    char sub[32];
    snprintf(sub, sizeof(sub), "sensor/%s/state", c->id);
    sa_mqtt_publish(sub, "", true);
}

static void publish_values(int i, bool include_values)
{
    const sa_sensor_cfg_t *c = &s_cfg[i];
    const runtime_t *r = &s_rt[i];
    char sub[48], val[24];
    char json[256];
    int pos = snprintf(json, sizeof(json), "{");
    for (int k = 0; k < r->nq; k++) {
        if (!r->valid[k]) {
            continue;
        }
        sa_quantity_t q = r->q[k];
        float med = sa_median(r->hist[k], r->hist_n[k]);
        snprintf(val, sizeof(val), fmt_for(q), r->last[k]);
        if (include_values) {
            snprintf(sub, sizeof(sub), "sensor/%s/%s", c->id, sa_quantity_name(q));
            sa_mqtt_publish(sub, val, true);
        }
        char medv[24];
        snprintf(medv, sizeof(medv), fmt_for(q), med);
        pos += snprintf(json + pos, sizeof(json) - pos, "\"%s\":%s,\"%s_median\":%s,",
                        sa_quantity_name(q), val, sa_quantity_name(q), medv);
    }
    snprintf(json + pos, sizeof(json) - pos, "\"ok\":%s,\"state\":\"%s\"}",
             r->state == SA_SENSOR_OK ? "true" : "false", sa_sensor_state_name(r->state));
    snprintf(sub, sizeof(sub), "sensor/%s/state", c->id);
    sa_mqtt_publish(sub, json, true);
}

// Called from the MQTT task, which holds the MQTT client lock while dispatching events.
// Taking the sensor lock here could deadlock with the sensor task (which publishes while
// holding the sensor lock), so the announcement is handed over to the sensor task.
void sensors_mqtt_announce(void)
{
    s_announce_pending = true;
}

static void announce_all(void)
{
    lock();
    for (int i = 0; i < SA_MAX_SENSORS; i++) {
        if (s_cfg[i].used) {
            announce(i);
            if (s_cfg[i].active) {
                publish_values(i, true);
            }
        }
    }
    unlock();
}

// ---------------------------------------------------------------- reading

static bool driver_init(int i)
{
    const sa_sensor_cfg_t *c = &s_cfg[i];
    runtime_t *r = &s_rt[i];
    i2c_master_bus_handle_t bus = sa_i2c_bus();
    if (!bus) {
        r->state = SA_SENSOR_MISSING;
        strlcpy(r->error, "I2C bus not available", sizeof(r->error));
        return false;
    }
    switch ((sa_chip_t)c->chip) {
    case SA_CHIP_VL53L1X: {
        sa_i2c_lock();
        esp_err_t err = vl53l1x_init(&r->vl, bus, c->addr, c->vl_mode == 1 ? VL53L1X_SHORT : VL53L1X_LONG);
        if (err != ESP_OK) {
            sa_i2c_recover();   // the device is detached again; rebuild the bus cleanly
        }
        sa_i2c_unlock();
        if (err != ESP_OK) {
            r->state = SA_SENSOR_MISSING;
            snprintf(r->error, sizeof(r->error), "not found at 0x%02X (%s)", c->addr, esp_err_to_name(err));
            return false;
        }
        r->inited = true;
        return true;
    }
    default:
        r->state = SA_SENSOR_UNSUPPORTED;
        snprintf(r->error, sizeof(r->error), "no driver for %s yet", sa_chip_name((sa_chip_t)c->chip));
        return false;
    }
}

// Reads raw values into out[] (in r->q order). Returns false with r->error set on failure.
static bool driver_read(int i, float out[SA_MAX_QUANTITIES])
{
    const sa_sensor_cfg_t *c = &s_cfg[i];
    runtime_t *r = &s_rt[i];
    switch ((sa_chip_t)c->chip) {
    case SA_CHIP_VL53L1X: {
        uint16_t mm = 0;
        uint8_t st = 255;
        sa_i2c_lock();
        esp_err_t err = vl53l1x_read(&r->vl, &mm, &st);
        sa_i2c_unlock();
        if (err != ESP_OK) {
            snprintf(r->error, sizeof(r->error), "read failed: %s", esp_err_to_name(err));
            return false;
        }
        if (st != 0) {
            snprintf(r->error, sizeof(r->error), "%s", vl53l1x_status_text(st));
            return false;
        }
        float d = mm + c->offset[SA_Q_DISTANCE];
        for (int k = 0; k < r->nq; k++) {
            out[k] = r->q[k] == SA_Q_DISTANCE ? d : (float)c->mount_mm - d + c->offset[SA_Q_LEVEL];
        }
        return true;
    }
    default:
        return false;
    }
}

static void read_one(int i, int64_t now)
{
    const sa_sensor_cfg_t *c = &s_cfg[i];
    runtime_t *r = &s_rt[i];
    r->next_due_us = now + (int64_t)c->interval_s * 1000000;

    if (!r->inited) {
        sa_sensor_state_t before = r->state;
        if (!driver_init(i)) {
            if (before == SA_SENSOR_ERROR) {
                r->state = SA_SENSOR_ERROR;   // keep reporting the outage as one error
                r->next_due_us = now + RETRY_ERROR_US;
                return;
            }
            if (before != r->state) {
                ESP_LOGW(TAG, "%s: %s", c->id, r->error);
                if (s_value_cb) {
                    for (int k = 0; k < r->nq; k++) {
                        s_value_cb(c->id, r->q[k], NAN, NAN, true);
                    }
                }
            }
            r->next_due_us = now + RETRY_INIT_US;
            if (r->state == SA_SENSOR_UNSUPPORTED) {
                r->next_due_us = INT64_MAX;   // until the configuration changes
            }
            return;
        }
        if (before != SA_SENSOR_ERROR) {
            r->state = SA_SENSOR_STARTING;
        }
        r->error[0] = '\0';
    }

    float v[SA_MAX_QUANTITIES];
    if (!driver_read(i, v)) {
        r->fails++;
        if (r->fails >= FAILS_TO_ERROR && r->state == SA_SENSOR_ERROR) {
            // Still failing: release and re-initialise the sensor (it may have been
            // unplugged and lost its configuration).
            sa_i2c_lock();
            if (r->vl.dev) {
                i2c_master_bus_rm_device(r->vl.dev);   // it does not answer: no stop command
                r->vl.dev = NULL;
            }
            sa_i2c_recover();
            sa_i2c_unlock();
            r->inited = false;
            r->next_due_us = now + RETRY_ERROR_US;
            return;
        }
        if (r->fails >= FAILS_TO_ERROR && r->state != SA_SENSOR_ERROR) {
            r->state = SA_SENSOR_ERROR;
            ESP_LOGW(TAG, "%s: %d failed reads in a row: %s", c->id, r->fails, r->error);
            publish_values(i, false);
            if (s_value_cb) {
                for (int k = 0; k < r->nq; k++) {
                    s_value_cb(c->id, r->q[k], NAN, NAN, true);
                }
            }
        }
        return;
    }
    if (r->state == SA_SENSOR_ERROR) {
        ESP_LOGI(TAG, "%s: readings are valid again", c->id);
    } else if (r->state == SA_SENSOR_STARTING) {
        ESP_LOGI(TAG, "%s: first reading %.0f %s", c->id, v[0], sa_quantity_unit(r->q[0]));
    }
    r->fails = 0;
    r->state = SA_SENSOR_OK;
    r->error[0] = '\0';
    for (int k = 0; k < r->nq; k++) {
        r->last[k] = v[k];
        r->last_us[k] = now;
        r->valid[k] = true;
        r->hist[k][r->hist_pos[k]] = v[k];
        r->hist_pos[k] = (r->hist_pos[k] + 1) % c->median_n;
        if (r->hist_n[k] < c->median_n) {
            r->hist_n[k]++;
        }
    }
    publish_values(i, true);
    if (s_value_cb) {
        for (int k = 0; k < r->nq; k++) {
            s_value_cb(c->id, r->q[k], r->last[k], sa_median(r->hist[k], r->hist_n[k]), false);
        }
    }
}

static void task(void *arg)
{
    for (;;) {
        if (s_announce_pending) {
            s_announce_pending = false;
            announce_all();
        }
        int64_t now = esp_timer_get_time();
        lock();
        for (int i = 0; i < SA_MAX_SENSORS; i++) {
            if (s_cfg[i].used && s_cfg[i].active && now >= s_rt[i].next_due_us) {
                read_one(i, now);
            }
        }
        unlock();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------------------------------------------------------------- public API

esp_err_t sensors_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    load();
    int n = 0;
    for (int i = 0; i < SA_MAX_SENSORS; i++) {
        if (s_cfg[i].used) {
            reset_runtime(i);
            n++;
        }
    }
    ESP_LOGI(TAG, "%d sensor(s) configured", n);
    sa_mqtt_add_connect_handler(sensors_mqtt_announce);
    return xTaskCreate(task, "sensors", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}

static void fill_info(int i, sa_sensor_info_t *o, int64_t now)
{
    const runtime_t *r = &s_rt[i];
    memset(o, 0, sizeof(*o));
    o->cfg = s_cfg[i];
    o->state = s_cfg[i].active ? r->state : SA_SENSOR_INACTIVE;
    strlcpy(o->error, r->error, sizeof(o->error));
    o->nq = r->nq;
    for (int k = 0; k < r->nq; k++) {
        o->q[k] = r->q[k];
        o->v[k].valid = r->valid[k];
        o->v[k].stale = r->state == SA_SENSOR_ERROR || r->state == SA_SENSOR_MISSING;
        o->v[k].value = r->last[k];
        o->v[k].median = sa_median(r->hist[k], r->hist_n[k]);
        o->v[k].age_ms = r->valid[k] ? (now - r->last_us[k]) / 1000 : 0;
    }
}

int sensors_list(sa_sensor_info_t *out, int max)
{
    int n = 0;
    int64_t now = esp_timer_get_time();
    lock();
    for (int i = 0; i < SA_MAX_SENSORS && n < max; i++) {
        if (s_cfg[i].used) {
            fill_info(i, &out[n++], now);
        }
    }
    unlock();
    return n;
}

bool sensors_get(const char *id, sa_sensor_info_t *out)
{
    lock();
    int i = find(id);
    if (i >= 0) {
        fill_info(i, out, esp_timer_get_time());
    }
    unlock();
    return i >= 0;
}

esp_err_t sensors_add(uint8_t chip, uint8_t addr, char id_out[SA_ID_MAX + 1], const char **why)
{
    *why = NULL;
    lock();
    int slot = -1;
    for (int i = 0; i < SA_MAX_SENSORS; i++) {
        if (s_cfg[i].used && s_cfg[i].addr == addr) {
            unlock();
            *why = "A sensor with this address is already configured";
            return ESP_ERR_INVALID_STATE;
        }
        if (!s_cfg[i].used && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        unlock();
        *why = "All 8 sensor slots are in use";
        return ESP_ERR_NO_MEM;
    }
    sa_sensor_cfg_t c;
    for (int n = 1; n < 100; n++) {
        sa_sensor_defaults(&c, chip, addr, n);
        if (find(c.id) < 0) {
            break;
        }
    }
    s_cfg[slot] = c;
    reset_runtime(slot);
    esp_err_t err = save();
    if (err == ESP_OK) {
        strlcpy(id_out, c.id, SA_ID_MAX + 1);
        announce(slot);
        ESP_LOGI(TAG, "added %s (%s at 0x%02X)", c.id, sa_chip_name((sa_chip_t)chip), addr);
    } else {
        s_cfg[slot].used = false;
        *why = "Saving failed";
    }
    unlock();
    return err;
}

esp_err_t sensors_update(const char *id, const sa_sensor_cfg_t *in, const char **why)
{
    *why = sa_sensor_validate(in);
    if (*why) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    int i = find(id);
    if (i < 0) {
        unlock();
        *why = "Unknown sensor";
        return ESP_ERR_NOT_FOUND;
    }
    int other = find(in->id);
    if (other >= 0 && other != i) {
        unlock();
        *why = "ID: already used by another sensor";
        return ESP_ERR_INVALID_ARG;
    }
    sa_sensor_cfg_t old = s_cfg[i];
    sa_sensor_cfg_t c = *in;
    c.used = true;
    c.chip = old.chip;     // hardware identity is fixed
    c.addr = old.addr;

    bool reinit = c.vl_mode != old.vl_mode || c.active != old.active;
    bool requant = c.mount_mm != old.mount_mm && (c.mount_mm == 0 || old.mount_mm == 0);
    bool renamed = strcmp(c.id, old.id) != 0;
    if (renamed || requant || !c.active) {
        unannounce(&old);
    }
    s_cfg[i] = c;
    if (reinit || requant || c.median_n != old.median_n) {
        reset_runtime(i);
    } else {
        s_rt[i].next_due_us = 0;   // read again soon with the new settings
    }
    esp_err_t err = save();
    if (err != ESP_OK) {
        s_cfg[i] = old;
        reset_runtime(i);
        *why = "Saving failed";
    } else {
        announce(i);
        ESP_LOGI(TAG, "%s: settings changed", c.id);
    }
    unlock();
    return err;
}

esp_err_t sensors_remove(const char *id)
{
    lock();
    int i = find(id);
    if (i < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    unannounce(&s_cfg[i]);
    sa_sensor_cfg_t old = s_cfg[i];
    reset_runtime(i);
    memset(&s_cfg[i], 0, sizeof(s_cfg[i]));
    memset(&s_rt[i], 0, sizeof(s_rt[i]));
    esp_err_t err = save();
    if (err != ESP_OK) {
        s_cfg[i] = old;
        reset_runtime(i);
    } else {
        ESP_LOGI(TAG, "removed %s", old.id);
    }
    unlock();
    return err;
}

int sensors_scan(sa_scan_entry_t *out, int max)
{
    sa_i2c_found_t found[16];
    int n = sa_i2c_scan(found, 16);
    if (n > 16) {
        n = 16;
    }
    int m = 0;
    lock();
    for (int k = 0; k < n && m < max; k++) {
        sa_scan_entry_t *e = &out[m++];
        memset(e, 0, sizeof(*e));
        e->dev = found[k];
        for (int i = 0; i < SA_MAX_SENSORS; i++) {
            if (s_cfg[i].used && s_cfg[i].addr == found[k].addr) {
                e->configured = true;
                strlcpy(e->sensor_id, s_cfg[i].id, sizeof(e->sensor_id));
            }
        }
    }
    unlock();
    return m;
}
