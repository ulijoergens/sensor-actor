// Pure helpers of the sensor framework (no I/O), unit-tested.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sensors.h"

static const char *const Q_NAME[SA_Q_COUNT] = {"temperature", "humidity", "pressure", "distance", "level"};
static const char *const Q_UNIT[SA_Q_COUNT] = {"°C", "%", "hPa", "mm", "mm"};

const char *sa_quantity_name(sa_quantity_t q)
{
    return q < SA_Q_COUNT ? Q_NAME[q] : "?";
}

const char *sa_quantity_unit(sa_quantity_t q)
{
    return q < SA_Q_COUNT ? Q_UNIT[q] : "";
}

const char *sa_sensor_state_name(sa_sensor_state_t s)
{
    switch (s) {
    case SA_SENSOR_OK: return "ok";
    case SA_SENSOR_STARTING: return "starting";
    case SA_SENSOR_INACTIVE: return "inactive";
    case SA_SENSOR_ERROR: return "error";
    case SA_SENSOR_MISSING: return "missing";
    case SA_SENSOR_UNSUPPORTED: return "unsupported";
    default: return "?";
    }
}

float sa_median(const float *v, int n)
{
    float tmp[SA_MEDIAN_MAX];
    if (n <= 0) {
        return NAN;
    }
    if (n > SA_MEDIAN_MAX) {
        n = SA_MEDIAN_MAX;
    }
    for (int i = 0; i < n; i++) {           // insertion sort into tmp
        float x = v[i];
        int j = i;
        while (j > 0 && tmp[j - 1] > x) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = x;
    }
    return (n & 1) ? tmp[n / 2] : (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0f;
}

int sa_sensor_quantities(const sa_sensor_cfg_t *c, sa_quantity_t out[SA_MAX_QUANTITIES])
{
    int n = 0;
    switch ((sa_chip_t)c->chip) {
    case SA_CHIP_AHT20:
    case SA_CHIP_SHT2X:
    case SA_CHIP_SHT3X:
        out[n++] = SA_Q_TEMPERATURE;
        out[n++] = SA_Q_HUMIDITY;
        break;
    case SA_CHIP_BME280:
        out[n++] = SA_Q_TEMPERATURE;
        out[n++] = SA_Q_HUMIDITY;
        out[n++] = SA_Q_PRESSURE;
        break;
    case SA_CHIP_DS18B20:
        out[n++] = SA_Q_TEMPERATURE;
        break;
    case SA_CHIP_VL53L0X:
    case SA_CHIP_VL53L1X:
        out[n++] = SA_Q_DISTANCE;
        if (c->mount_mm > 0) {
            out[n++] = SA_Q_LEVEL;   // REQ-SEN-7
        }
        break;
    default:
        break;
    }
    return n;
}

void sa_rom_to_str(uint64_t rom, char out[17])
{
    for (int i = 0; i < 8; i++) {   // byte 0 (family code) first
        snprintf(out + 2 * i, 3, "%02X", (unsigned)((rom >> (8 * i)) & 0xFF));
    }
}

bool sa_rom_from_str(const char *s, uint64_t *rom)
{
    if (strlen(s) != 16) {
        return false;
    }
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) {
        unsigned b;
        char hex[3] = {s[2 * i], s[2 * i + 1], 0};
        if (!isxdigit((unsigned char)hex[0]) || !isxdigit((unsigned char)hex[1]) || sscanf(hex, "%x", &b) != 1) {
            return false;
        }
        r |= (uint64_t)b << (8 * i);
    }
    *rom = r;
    return true;
}

void sa_sensor_defaults(sa_sensor_cfg_t *c, uint8_t chip, uint8_t addr, uint64_t rom, int n)
{
    memset(c, 0, sizeof(*c));
    c->used = true;
    c->chip = chip;
    c->addr = addr;
    const char *base = (chip == SA_CHIP_VL53L0X || chip == SA_CHIP_VL53L1X) ? "dist"
                     : chip == SA_CHIP_DS18B20 ? "temp"
                     : chip == SA_CHIP_BME280 ? "env" : "th";
    snprintf(c->id, sizeof(c->id), "%s%d", base, n);
    if (chip == SA_CHIP_DS18B20) {
        char r[17];
        sa_rom_to_str(rom, r);
        c->rom = rom;
        c->ds_resolution = 12;
        snprintf(c->label, sizeof(c->label), "DS18B20 %.6s", r + 10);   // last bytes: serial + CRC
    } else {
        snprintf(c->label, sizeof(c->label), "%s 0x%02X", sa_chip_name((sa_chip_t)chip), addr);
    }
    c->active = true;
    c->interval_s = 10;
    c->median_n = 15;
    c->vl_mode = 2;
}

const char *sa_sensor_validate(const sa_sensor_cfg_t *c)
{
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
    if (c->interval_s < 1 || c->interval_s > 3600) {
        return "Read interval: 1 to 3600 s";
    }
    if (c->median_n < 3 || c->median_n > SA_MEDIAN_MAX) {
        return "Median window: 3 to 31 samples";
    }
    for (int q = 0; q < SA_Q_COUNT; q++) {
        if (!isfinite(c->offset[q]) || fabsf(c->offset[q]) > 10000.0f) {
            return "Offset: a number between -10000 and 10000";
        }
    }
    if (c->chip == SA_CHIP_DS18B20) {
        if ((c->rom & 0xFF) != 0x28) {
            return "1-Wire ROM code is not a DS18B20 (family 28)";
        }
        if (c->ds_resolution < 9 || c->ds_resolution > 12) {
            return "Resolution: 9 to 12 bit";
        }
    }
    if (c->chip == SA_CHIP_VL53L0X || c->chip == SA_CHIP_VL53L1X) {
        if (c->vl_mode != 1 && c->vl_mode != 2) {
            return "Distance mode: short or long";
        }
        if (c->mount_mm > 4000) {
            return "Mounting height: 0 (off) to 4000 mm";
        }
    }
    return NULL;
}
