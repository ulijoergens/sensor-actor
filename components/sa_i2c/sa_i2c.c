// I2C bus and sensor detection, see sa_i2c.h.
#include "sa_i2c.h"

#include <stdio.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sa_config.h"

static const char *TAG = "i2c";

#define XFER_MS   50
#define SCAN_HZ   100000

static i2c_master_bus_handle_t s_bus;

const char *sa_chip_name(sa_chip_t c)
{
    switch (c) {
    case SA_CHIP_AHT20: return "AHT20";
    case SA_CHIP_SHT2X: return "SHT2x (SHT25)";
    case SA_CHIP_SHT3X: return "SHT3x (SHT35)";
    case SA_CHIP_BME280: return "BME280";
    case SA_CHIP_BMP280: return "BMP280";
    case SA_CHIP_BME680: return "BME680";
    case SA_CHIP_VL53L0X: return "VL53L0X";
    case SA_CHIP_VL53L1X: return "VL53L1X";
    default: return "unknown device";
    }
}

uint8_t sa_crc8_sensirion(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t sa_i2c_init(void)
{
    const sa_device_cfg_t *c = sa_config_get();
    i2c_master_bus_config_t bc = {
        .i2c_port = -1,
        .sda_io_num = c->i2c_sda,
        .scl_io_num = c->i2c_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bc, &s_bus);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "bus on SDA %d, SCL %d", c->i2c_sda, c->i2c_scl);
    } else {
        ESP_LOGE(TAG, "bus init on SDA %d, SCL %d failed: %s", c->i2c_sda, c->i2c_scl, esp_err_to_name(err));
    }
    return err;
}

// Temporary device handle for one identification step.
static i2c_master_dev_handle_t open_dev(uint8_t addr)
{
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = SCAN_HZ,
    };
    i2c_master_dev_handle_t d = NULL;
    if (i2c_master_bus_add_device(s_bus, &dc, &d) != ESP_OK) {
        return NULL;
    }
    return d;
}

static bool read_reg8(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(d, &reg, 1, val, 1, XFER_MS) == ESP_OK;
}

static void identify(sa_i2c_found_t *f)
{
    i2c_master_dev_handle_t d = open_dev(f->addr);
    f->chip = SA_CHIP_UNKNOWN;
    if (!d) {
        return;
    }
    uint8_t v = 0, buf[3];

    switch (f->addr) {
    case 0x38: {   // AHT20: status byte, bit 3 = calibrated
        if (i2c_master_receive(d, &v, 1, XFER_MS) == ESP_OK) {
            f->chip = SA_CHIP_AHT20;
            snprintf(f->detail, sizeof(f->detail), "status 0x%02X%s", v, (v & 0x08) ? ", calibrated" : "");
        }
        break;
    }
    case 0x40: {   // SHT2x: read user register (0xE7)
        if (read_reg8(d, 0xE7, &v)) {
            f->chip = SA_CHIP_SHT2X;
            snprintf(f->detail, sizeof(f->detail), "user reg 0x%02X", v);
        }
        break;
    }
    case 0x44:
    case 0x45: {   // SHT3x: read status (0xF32D) -> 2 bytes + CRC
        const uint8_t cmd[2] = {0xF3, 0x2D};
        if (i2c_master_transmit(d, cmd, 2, XFER_MS) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            if (i2c_master_receive(d, buf, 3, XFER_MS) == ESP_OK &&
                sa_crc8_sensirion(buf, 2) == buf[2]) {
                f->chip = SA_CHIP_SHT3X;
                snprintf(f->detail, sizeof(f->detail), "status 0x%02X%02X", buf[0], buf[1]);
            }
        }
        break;
    }
    case 0x76:
    case 0x77: {   // Bosch: chip ID register 0xD0
        if (read_reg8(d, 0xD0, &v)) {
            f->chip = v == 0x60 ? SA_CHIP_BME280 : v == 0x58 ? SA_CHIP_BMP280
                    : v == 0x61 ? SA_CHIP_BME680 : SA_CHIP_UNKNOWN;
            snprintf(f->detail, sizeof(f->detail), "chip ID 0x%02X", v);
        }
        break;
    }
    case 0x29: {   // VL53L1X: 16-bit register 0x010F = 0xEA; VL53L0X: register 0xC0 = 0xEE
        const uint8_t reg16[2] = {0x01, 0x0F};
        if (i2c_master_transmit_receive(d, reg16, 2, &v, 1, XFER_MS) == ESP_OK && v == 0xEA) {
            f->chip = SA_CHIP_VL53L1X;
            snprintf(f->detail, sizeof(f->detail), "model ID 0x%02X", v);
        } else if (read_reg8(d, 0xC0, &v) && v == 0xEE) {
            f->chip = SA_CHIP_VL53L0X;
            snprintf(f->detail, sizeof(f->detail), "model ID 0x%02X", v);
        } else {
            snprintf(f->detail, sizeof(f->detail), "ID read 0x%02X", v);
        }
        break;
    }
    default:
        break;
    }
    i2c_master_bus_rm_device(d);

    f->supported = f->chip == SA_CHIP_AHT20 || f->chip == SA_CHIP_SHT2X || f->chip == SA_CHIP_SHT3X ||
                   f->chip == SA_CHIP_BME280 || f->chip == SA_CHIP_VL53L0X || f->chip == SA_CHIP_VL53L1X;
}

int sa_i2c_scan(sa_i2c_found_t *out, int max)
{
    if (!s_bus) {
        return 0;
    }
    int n = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(s_bus, a, XFER_MS) != ESP_OK) {
            continue;
        }
        sa_i2c_found_t f = {.addr = a};
        identify(&f);
        ESP_LOGI(TAG, "0x%02X: %s%s%s", a, sa_chip_name(f.chip), f.detail[0] ? " - " : "", f.detail);
        if (n < max) {
            out[n] = f;
        }
        n++;
    }
    return n;
}
