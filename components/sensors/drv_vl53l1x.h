// VL53L1X time-of-flight distance sensor, minimal driver after ST's Ultra Lite Driver.
#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef enum {
    VL53L1X_SHORT = 1,   // up to ~1.3 m, more robust in ambient light
    VL53L1X_LONG = 2,    // up to ~4 m (default)
} vl53l1x_mode_t;

typedef struct {
    i2c_master_dev_handle_t dev;
} vl53l1x_t;

// Initialises the sensor and starts continuous ranging (100 ms timing budget).
esp_err_t vl53l1x_init(vl53l1x_t *s, i2c_master_bus_handle_t bus, uint8_t addr, vl53l1x_mode_t mode);

// Waits for the next measurement. `status` is the ST range status (0 = valid).
esp_err_t vl53l1x_read(vl53l1x_t *s, uint16_t *mm, uint8_t *status);

void vl53l1x_deinit(vl53l1x_t *s);

const char *vl53l1x_status_text(uint8_t status);
