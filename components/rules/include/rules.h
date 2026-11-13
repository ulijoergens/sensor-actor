// Threshold rules for actors (docs/FSD.md §4.1, REQ-RUL-1..8).
//
// A rule watches one sensor value v and has up to two thresholds. Each threshold has an
// action for a rising crossing (previous v <= T, new v > T) and one for a falling
// crossing (previous v > T, new v <= T). Rules are edge-triggered: they only act when a
// threshold is crossed, so manual commands stay in effect until the next crossing.
//
// The engine is pure C without ESP-IDF dependencies so it can be unit-tested anywhere.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RULE_MAX_THRESHOLDS 2

typedef enum {
    RULE_ACTION_NONE = 0,
    RULE_ACTION_ON,
    RULE_ACTION_OFF,
} rule_action_t;

typedef struct {
    bool enabled;
    float value;              // threshold T
    rule_action_t rising;     // v goes from <= T to > T
    rule_action_t falling;    // v goes from > T to <= T
} rule_threshold_t;

typedef struct {
    rule_threshold_t th[RULE_MAX_THRESHOLDS];
    rule_action_t fault;      // applied once when the sensor fails (REQ-RUL-8)
} rule_config_t;

// Runtime state, one per rule. Zero-initialise (or call rule_state_reset()).
typedef struct {
    bool has_prev;            // false after boot and after a sensor fault
    float prev;
    bool faulted;
} rule_state_t;

void rule_state_reset(rule_state_t *st);

// true if at least one enabled threshold has an action (REQ-RUL-7).
bool rule_config_valid(const rule_config_t *cfg);

// Feed a new valid sample. Returns the action to apply (the last one wins if a single
// sample crosses both thresholds), or RULE_ACTION_NONE.
//  - first sample after reset/fault: applies the action for the side v is on, for each
//    threshold in ascending threshold order (REQ-RUL-5);
//  - otherwise: actions of crossed thresholds, in the order of the movement (REQ-RUL-4).
rule_action_t rule_update(const rule_config_t *cfg, rule_state_t *st, float v);

// Report that the sensor is in error or stale. Returns the fault action the first time
// it is called after a valid sample, RULE_ACTION_NONE afterwards (REQ-RUL-6/8). The next
// valid sample is treated as a first sample (REQ-RUL-5).
rule_action_t rule_fault(const rule_config_t *cfg, rule_state_t *st);

// Human-readable summary, e.g. "OFF when v rises above 30; ON when v falls to 28 or
// below" (REQ-RUL-7). `quantity` names the value, e.g. "temperature". Returns the number
// of characters written (excluding the terminator), truncated to fit `len`.
int rule_describe(const rule_config_t *cfg, const char *quantity, char *buf, int len);

#ifdef __cplusplus
}
#endif
