// I2C bus and sensor detection (docs/FSD.md §2.1, REQ-DET-1).
//
// Probes every 7-bit address and identifies the supported chips by address and chip-ID
// register: AHT20, SHT2x, SHT3x, BME280 (BMP280/BME680 are reported as unsupported),
// VL53L0X, VL53L1X.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SA_CHIP_UNKNOWN = 0,   // something answers, but not a supported chip
    SA_CHIP_AHT20,
    SA_CHIP_SHT2X,         // SHT25 (also HTU21D / Si7021 at 0x40)
    SA_CHIP_SHT3X,         // SHT35 (SHT30/31 too)
    SA_CHIP_BME280,
    SA_CHIP_BMP280,        // unsupported: no humidity
    SA_CHIP_BME680,        // unsupported
    SA_CHIP_VL53L0X,
    SA_CHIP_VL53L1X,
} sa_chip_t;

typedef struct {
    uint8_t addr;
    sa_chip_t chip;
    bool supported;
    char detail[32];       // e.g. "chip ID 0x60"
} sa_i2c_found_t;

const char *sa_chip_name(sa_chip_t c);

// Sensirion CRC-8 (polynomial 0x31, init 0xFF), used by SHT2x/SHT3x/AHT20.
uint8_t sa_crc8_sensirion(const uint8_t *data, int len);

// Creates the bus on the configured SDA/SCL pins (internal pull-ups enabled; external
// 4.7 kΩ pull-ups are still recommended).
esp_err_t sa_i2c_init(void);

// The shared bus handle (NULL if init failed), for sensor drivers.
i2c_master_bus_handle_t sa_i2c_bus(void);

// Serialises bus access between the sensor task and scans.
void sa_i2c_lock(void);
void sa_i2c_unlock(void);

// Scans 0x08..0x77 and identifies what it finds. Returns the number of devices (up to
// `max` are stored).
int sa_i2c_scan(sa_i2c_found_t *out, int max);

#ifdef __cplusplus
}
#endif
