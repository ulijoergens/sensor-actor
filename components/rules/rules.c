// Threshold rule engine, see rules.h and docs/FSD.md §4.1.
#include "rules.h"

#include <stdio.h>
#include <string.h>

void rule_state_reset(rule_state_t *st)
{
    memset(st, 0, sizeof(*st));
}

bool rule_config_valid(const rule_config_t *cfg)
{
    for (int i = 0; i < RULE_MAX_THRESHOLDS; i++) {
        const rule_threshold_t *t = &cfg->th[i];
        if (t->enabled && (t->rising != RULE_ACTION_NONE || t->falling != RULE_ACTION_NONE)) {
            return true;
        }
    }
    return false;
}

// Indices of the enabled thresholds sorted by ascending threshold value.
static int sorted_thresholds(const rule_config_t *cfg, int idx[RULE_MAX_THRESHOLDS])
{
    int n = 0;
    for (int i = 0; i < RULE_MAX_THRESHOLDS; i++) {
        if (!cfg->th[i].enabled) {
            continue;
        }
        int j = n++;
        while (j > 0 && cfg->th[idx[j - 1]].value > cfg->th[i].value) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = i;
    }
    return n;
}

static void apply(rule_action_t *result, rule_action_t a)
{
    if (a != RULE_ACTION_NONE) {
        *result = a;   // last action wins
    }
}

rule_action_t rule_update(const rule_config_t *cfg, rule_state_t *st, float v)
{
    int idx[RULE_MAX_THRESHOLDS];
    int n = sorted_thresholds(cfg, idx);
    rule_action_t result = RULE_ACTION_NONE;

    if (!st->has_prev) {
        // REQ-RUL-5: act as if v had just arrived on its current side of every threshold.
        for (int k = 0; k < n; k++) {
            const rule_threshold_t *t = &cfg->th[idx[k]];
            apply(&result, v > t->value ? t->rising : t->falling);
        }
    } else if (v > st->prev) {
        // Rising: thresholds crossed upwards, lowest first.
        for (int k = 0; k < n; k++) {
            const rule_threshold_t *t = &cfg->th[idx[k]];
            if (st->prev <= t->value && v > t->value) {
                apply(&result, t->rising);
            }
        }
    } else if (v < st->prev) {
        // Falling: thresholds crossed downwards, highest first.
        for (int k = n - 1; k >= 0; k--) {
            const rule_threshold_t *t = &cfg->th[idx[k]];
            if (st->prev > t->value && v <= t->value) {
                apply(&result, t->falling);
            }
        }
    }

    st->has_prev = true;
    st->prev = v;
    st->faulted = false;
    return result;
}

rule_action_t rule_fault(const rule_config_t *cfg, rule_state_t *st)
{
    bool first = !st->faulted;
    st->faulted = true;
    st->has_prev = false;   // recovery is handled like a first sample (REQ-RUL-5)
    return first ? cfg->fault : RULE_ACTION_NONE;
}

static const char *action_name(rule_action_t a)
{
    return a == RULE_ACTION_ON ? "ON" : "OFF";
}

int rule_describe(const rule_config_t *cfg, const char *quantity, char *buf, int len)
{
    int idx[RULE_MAX_THRESHOLDS];
    int n = sorted_thresholds(cfg, idx);
    int pos = 0;
    if (len <= 0) {
        return 0;
    }
    buf[0] = '\0';

    for (int k = 0; k < n; k++) {
        const rule_threshold_t *t = &cfg->th[idx[k]];
        const rule_action_t acts[2] = {t->rising, t->falling};
        for (int d = 0; d < 2; d++) {
            if (acts[d] == RULE_ACTION_NONE) {
                continue;
            }
            int w = snprintf(buf + pos, len - pos,
                             d == 0 ? "%s%s when %s rises above %g" : "%s%s when %s falls to %g or below",
                             pos ? "; " : "", action_name(acts[d]), quantity, (double)t->value);
            if (w < 0) {
                return pos;
            }
            pos += w;
            if (pos >= len) {
                return len - 1;
            }
        }
    }
    if (pos == 0) {
        pos = snprintf(buf, len, "no action");
        if (pos >= len) {
            pos = len - 1;
        }
    }
    return pos;
}
