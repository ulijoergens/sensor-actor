// Actors: relays on GPIO pins with optional threshold rules (docs/FSD.md §4, §4.1).
//
// State changes come from the web UI, MQTT (sensact/<id>/actor/<aid>/set) and rules. All
// state changes run in the actor task (MQTT commands and sensor values arrive through a
// queue), so no lock is ever held across modules.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "rules.h"
#include "sa_config.h"
#include "sensors.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SA_MAX_ACTORS 8

typedef enum {
    SA_POWER_ON_OFF = 0,
    SA_POWER_ON_ON,
    SA_POWER_ON_LAST,
} sa_power_on_t;

typedef struct {
    bool used;
    char id[SA_ID_MAX + 1];
    char label[SA_NAME_MAX + 1];
    int8_t gpio;
    bool active;
    bool favourite;
    bool active_low;              // relay switches on LOW (REQ-HW-3)
    uint8_t power_on;             // sa_power_on_t
    bool last_on;                 // stored state for SA_POWER_ON_LAST
    // Rule (REQ-RUL-1..8)
    bool rule_enabled;
    char rule_sensor[SA_ID_MAX + 1];
    uint8_t rule_quantity;        // sa_quantity_t
    bool rule_use_median;
    rule_config_t rule;           // thresholds + fault action
} sa_actor_cfg_t;

typedef struct {
    sa_actor_cfg_t cfg;
    bool on;
    int level;                    // GPIO pad level read back (-1 when inactive)
    char source[8];               // what set the current state: boot, web, mqtt, rule, fault
    int64_t changed_s;            // seconds since the last change
    bool rule_waiting;            // rule enabled but no valid value seen yet
    bool rule_faulted;            // rule sensor in error
    bool rule_sensor_missing;     // rule refers to an unknown sensor/quantity
    char rule_text[160];          // plain-language summary
} sa_actor_info_t;

// ---- pure helpers (unit-tested)

int sa_actor_gpio_level(bool on, bool active_low);

void sa_actor_defaults(sa_actor_cfg_t *c, int gpio, int n);

// Validates an actor against the device pins and the other actors (others[] may contain
// the actor itself at index `self`, which is skipped). Returns NULL or a message.
const char *sa_actor_validate(const sa_actor_cfg_t *c, const sa_device_cfg_t *dev,
                              const sa_actor_cfg_t *others, int n_others, int self);

// Parses "ON"/"OFF"/"TOGGLE" (case-insensitive, surrounding whitespace ignored).
// Returns 1 = on, 0 = off, 2 = toggle, -1 = invalid.
int sa_actor_parse_command(const char *s);

// ---- runtime

esp_err_t actors_init(void);       // after sensors_init() and before sa_mqtt_start()
int actors_list(sa_actor_info_t *out, int max);
bool actors_get(const char *id, sa_actor_info_t *out);
esp_err_t actors_add(int gpio, char id_out[SA_ID_MAX + 1], const char **why);
esp_err_t actors_update(const char *id, const sa_actor_cfg_t *cfg, const char **why);
esp_err_t actors_remove(const char *id);

// Switch an actor: cmd = 1 on, 0 off, 2 toggle. `source` is shown in the UI ("web").
esp_err_t actors_command(const char *id, int cmd, const char *source, const char **why);

#ifdef __cplusplus
}
#endif
