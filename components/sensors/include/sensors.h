// Sensors (docs/FSD.md §3, REQ-SEN-1..7, REQ-DET-2).
//
// Configured sensors are stored in NVS and read by a background task at their own
// interval. Each quantity keeps its last value, a timestamp and the median of the last
// N valid samples. After 3 failed reads in a row a sensor is in error and its values are
// stale. Values are published over MQTT (with discovery).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sa_config.h"
#include "sa_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SA_MAX_SENSORS    8
#define SA_MAX_QUANTITIES 4
#define SA_MEDIAN_MAX     31

typedef enum {
    SA_Q_TEMPERATURE = 0,
    SA_Q_HUMIDITY,
    SA_Q_PRESSURE,
    SA_Q_DISTANCE,
    SA_Q_LEVEL,
    SA_Q_COUNT,
} sa_quantity_t;

const char *sa_quantity_name(sa_quantity_t q);   // "temperature", "level", ...
const char *sa_quantity_unit(sa_quantity_t q);   // "°C", "mm", ...

typedef enum {
    SA_SENSOR_OK = 0,
    SA_SENSOR_STARTING,    // configured, no reading yet
    SA_SENSOR_INACTIVE,
    SA_SENSOR_ERROR,       // 3+ failed reads in a row, values stale
    SA_SENSOR_MISSING,     // init failed: not found on the bus
    SA_SENSOR_UNSUPPORTED, // chip has no driver yet
} sa_sensor_state_t;

const char *sa_sensor_state_name(sa_sensor_state_t s);

typedef struct {
    bool used;
    uint8_t chip;                  // sa_chip_t
    uint8_t addr;                  // I2C address
    char id[SA_ID_MAX + 1];
    char label[SA_NAME_MAX + 1];
    bool active;
    bool favourite;
    uint16_t interval_s;           // 1..3600
    uint8_t median_n;              // 3..31
    float offset[SA_Q_COUNT];      // calibration per quantity
    uint8_t vl_mode;               // VL53Lx: 1 = short, 2 = long
    uint16_t mount_mm;             // VL53Lx: mounting height, 0 = no level quantity
} sa_sensor_cfg_t;

typedef struct {
    bool valid;                    // at least one valid sample
    bool stale;                    // last reads failed
    float value;
    float median;
    int64_t age_ms;                // since the last valid sample
} sa_qty_value_t;

typedef struct {
    sa_sensor_cfg_t cfg;
    sa_sensor_state_t state;
    char error[48];
    uint8_t nq;
    sa_quantity_t q[SA_MAX_QUANTITIES];
    sa_qty_value_t v[SA_MAX_QUANTITIES];
} sa_sensor_info_t;

// ---- pure helpers (unit-tested)

// Median of n values (n >= 1); the input is not modified.
float sa_median(const float *v, int n);

// Quantities a chip provides, given the configuration (level only with mount height).
int sa_sensor_quantities(const sa_sensor_cfg_t *c, sa_quantity_t out[SA_MAX_QUANTITIES]);

// Defaults for a newly found sensor; `n` makes the ID unique (e.g. "dist1").
void sa_sensor_defaults(sa_sensor_cfg_t *c, uint8_t chip, uint8_t addr, int n);

// NULL if valid, else a message naming the field.
const char *sa_sensor_validate(const sa_sensor_cfg_t *c);

// ---- runtime

esp_err_t sensors_init(void);      // load config, start the read task
int sensors_list(sa_sensor_info_t *out, int max);
bool sensors_get(const char *id, sa_sensor_info_t *out);

// Add a sensor found by a scan. Fills `id_out` with the assigned ID.
esp_err_t sensors_add(uint8_t chip, uint8_t addr, char id_out[SA_ID_MAX + 1], const char **why);
// Replace the configuration of sensor `id` (the ID itself may change).
esp_err_t sensors_update(const char *id, const sa_sensor_cfg_t *cfg, const char **why);
esp_err_t sensors_remove(const char *id);

// Scan the I2C bus; `configured` is set for devices that already have a sensor record.
typedef struct {
    sa_i2c_found_t dev;
    bool configured;
    char sensor_id[SA_ID_MAX + 1];
} sa_scan_entry_t;
int sensors_scan(sa_scan_entry_t *out, int max);

// Request discovery + current values for all sensors (MQTT connect handler; done by the
// sensor task).
void sensors_mqtt_announce(void);

#ifdef __cplusplus
}
#endif
