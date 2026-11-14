// Pure helpers of the actors module (no I/O), unit-tested.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "actors.h"

int sa_actor_gpio_level(bool on, bool active_low)
{
    return (on ^ active_low) ? 1 : 0;
}

void sa_actor_defaults(sa_actor_cfg_t *c, int gpio, int n)
{
    memset(c, 0, sizeof(*c));
    c->used = true;
    snprintf(c->id, sizeof(c->id), "relay%d", n);
    snprintf(c->label, sizeof(c->label), "Relay %d (GPIO %d)", n, gpio);
    c->gpio = (int8_t)gpio;
    c->active = true;
    c->power_on = SA_POWER_ON_OFF;
    c->rule_quantity = SA_Q_DISTANCE;
    c->rule_use_median = true;   // FSD REQ-RUL-1 default
}

int sa_actor_parse_command(const char *s)
{
    char buf[12];
    while (isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        n--;
    }
    if (n == 0 || n >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, s, n);
    buf[n] = '\0';
    if (!strcasecmp(buf, "ON") || !strcmp(buf, "1")) return 1;
    if (!strcasecmp(buf, "OFF") || !strcmp(buf, "0")) return 0;
    if (!strcasecmp(buf, "TOGGLE")) return 2;
    return -1;
}

const char *sa_actor_validate(const sa_actor_cfg_t *c, const sa_device_cfg_t *dev,
                              const sa_actor_cfg_t *others, int n_others, int self)
{
    static char msg[72];
    if (!sa_id_valid(c->id)) {
        return "ID: 1 to 8 characters a-z, 0-9, _ or -";
    }
    size_t n = strnlen(c->label, sizeof(c->label));
    if (n == 0 || n > SA_NAME_MAX) {
        return "Label: 1 to 32 characters";
    }
    for (size_t i = 0; i < n; i++) {
        if (!isprint((unsigned char)c->label[i]) && (unsigned char)c->label[i] < 0x80) {
            return "Label: no control characters";
        }
    }
    if (!sa_gpio_output_ok(c->gpio)) {
        snprintf(msg, sizeof(msg), "GPIO %d cannot drive an output (flash or input-only pin)", c->gpio);
        return msg;
    }
    if (c->gpio == dev->i2c_sda || c->gpio == dev->i2c_scl || c->gpio == dev->onewire) {
        snprintf(msg, sizeof(msg), "GPIO %d is used by the I2C or 1-Wire bus", c->gpio);
        return msg;
    }
    for (int i = 0; i < n_others; i++) {
        if (i == self || !others[i].used) {
            continue;
        }
        if (others[i].gpio == c->gpio) {
            snprintf(msg, sizeof(msg), "GPIO %d is already used by actor \"%s\"", c->gpio, others[i].id);
            return msg;
        }
        if (!strcmp(others[i].id, c->id)) {
            return "ID: already used by another actor";
        }
    }
    if (c->power_on > SA_POWER_ON_LAST) {
        return "Power-on state: off, on or last";
    }
    if (c->rule_enabled) {
        if (!sa_id_valid(c->rule_sensor)) {
            return "Rule: choose a sensor";
        }
        if (c->rule_quantity >= SA_Q_COUNT) {
            return "Rule: choose a value";
        }
        for (int t = 0; t < RULE_MAX_THRESHOLDS; t++) {
            if (c->rule.th[t].enabled && !isfinite(c->rule.th[t].value)) {
                return "Rule: thresholds must be numbers";
            }
            if (c->rule.th[t].rising > RULE_ACTION_OFF || c->rule.th[t].falling > RULE_ACTION_OFF) {
                return "Rule: actions are ON, OFF or none";
            }
        }
        if (c->rule.fault > RULE_ACTION_OFF) {
            return "Rule: fault action is ON, OFF or none";
        }
        if (!rule_config_valid(&c->rule)) {
            return "Rule: at least one threshold needs an action";
        }
    }
    return NULL;
}
