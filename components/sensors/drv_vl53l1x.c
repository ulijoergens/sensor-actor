// VL53L1X driver: register sequence and default configuration from ST's VL53L1X Ultra Lite
// Driver (STSW-IMG009, BSD-3-Clause).
#include "drv_vl53l1x.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "vl53l1x";

#define XFER_MS 50

#define REG_MODEL_ID              0x010F
#define REG_FIRMWARE_SYSTEM_STATUS 0x00E5
#define REG_GPIO_HV_MUX_CTRL      0x0030
#define REG_GPIO_TIO_HV_STATUS    0x0031
#define REG_SYSTEM_INTERRUPT_CLEAR 0x0086
#define REG_SYSTEM_MODE_START     0x0087
#define REG_RESULT_RANGE_STATUS   0x0089
#define REG_RESULT_DISTANCE       0x0096
#define REG_VHV_TIMEOUT_LOOP      0x0008
#define REG_VHV_START             0x000B
#define REG_PHASECAL_TIMEOUT      0x004B
#define REG_VCSEL_PERIOD_A        0x0060
#define REG_VCSEL_PERIOD_B        0x0063
#define REG_VALID_PHASE_HIGH      0x0069
#define REG_WOI_SD0               0x0078
#define REG_INITIAL_PHASE_SD0     0x007A
#define REG_TIMEOUT_MACROP_A      0x005E
#define REG_TIMEOUT_MACROP_B      0x0061

// Registers 0x2D..0x87 (ULD VL51L1X_DEFAULT_CONFIGURATION).
static const uint8_t DEFAULT_CONFIG[] = {
    0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x02, 0x08, 0x00, 0x08, 0x10, 0x01, 0x01, 0x00, 0x00, 0x00,
    0x00, 0xff, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x0b, 0x00, 0x00, 0x02, 0x0a, 0x21,
    0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x38, 0xff, 0x01, 0x00, 0x08, 0x00,
    0x00, 0x01, 0xcc, 0x0f, 0x01, 0xf1, 0x0d, 0x01, 0x68, 0x00, 0x80, 0x08, 0xb8, 0x00, 0x00, 0x00,
    0x00, 0x0f, 0x89, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0f, 0x0d, 0x0e, 0x0e, 0x00,
    0x00, 0x02, 0xc7, 0xff, 0x9B, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
};
_Static_assert(sizeof(DEFAULT_CONFIG) == 0x87 - 0x2D + 1, "default config covers 0x2D..0x87");

// ST range status -> ULD status (0 = valid).
static const uint8_t STATUS_MAP[24] = {255, 255, 255, 5, 2, 4, 1, 7, 3, 0, 255, 255,
                                       9, 13, 255, 255, 255, 255, 10, 6, 255, 255, 11, 12};

static esp_err_t wr(vl53l1x_t *s, uint16_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[2 + sizeof(DEFAULT_CONFIG)];
    if (len > sizeof(DEFAULT_CONFIG)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = reg >> 8;
    buf[1] = reg & 0xFF;
    memcpy(buf + 2, data, len);
    return i2c_master_transmit(s->dev, buf, len + 2, XFER_MS);
}

static esp_err_t wr8(vl53l1x_t *s, uint16_t reg, uint8_t v)
{
    return wr(s, reg, &v, 1);
}

static esp_err_t wr16(vl53l1x_t *s, uint16_t reg, uint16_t v)
{
    const uint8_t b[2] = {v >> 8, v & 0xFF};
    return wr(s, reg, b, 2);
}

static esp_err_t rd(vl53l1x_t *s, uint16_t reg, uint8_t *out, size_t len)
{
    const uint8_t r[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(s->dev, r, 2, out, len, XFER_MS);
}

static esp_err_t data_ready(vl53l1x_t *s, bool *ready)
{
    uint8_t mux, st;
    esp_err_t err = rd(s, REG_GPIO_HV_MUX_CTRL, &mux, 1);
    if (err == ESP_OK) {
        err = rd(s, REG_GPIO_TIO_HV_STATUS, &st, 1);
    }
    if (err == ESP_OK) {
        uint8_t polarity = !((mux & 0x10) >> 4);
        *ready = (st & 0x01) == polarity;
    }
    return err;
}

static esp_err_t wait_ready(vl53l1x_t *s, int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += 5) {
        bool ready = false;
        esp_err_t err = data_ready(s, &ready);
        if (err != ESP_OK) {
            return err;
        }
        if (ready) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t set_mode(vl53l1x_t *s, vl53l1x_mode_t mode)
{
    const bool lng = mode != VL53L1X_SHORT;
    esp_err_t err = ESP_OK;
    err |= wr8(s, REG_PHASECAL_TIMEOUT, lng ? 0x0A : 0x14);
    err |= wr8(s, REG_VCSEL_PERIOD_A, lng ? 0x0F : 0x07);
    err |= wr8(s, REG_VCSEL_PERIOD_B, lng ? 0x0D : 0x05);
    err |= wr8(s, REG_VALID_PHASE_HIGH, lng ? 0xB8 : 0x38);
    err |= wr16(s, REG_WOI_SD0, lng ? 0x0F0D : 0x0705);
    err |= wr16(s, REG_INITIAL_PHASE_SD0, lng ? 0x0E0E : 0x0606);
    // 100 ms timing budget for the selected mode
    err |= wr16(s, REG_TIMEOUT_MACROP_A, lng ? 0x01CC : 0x02E1);
    err |= wr16(s, REG_TIMEOUT_MACROP_B, lng ? 0x01EA : 0x0388);
    return err ? ESP_FAIL : ESP_OK;
}

esp_err_t vl53l1x_init(vl53l1x_t *s, i2c_master_bus_handle_t bus, uint8_t addr, vl53l1x_mode_t mode)
{
    memset(s, 0, sizeof(*s));
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &s->dev);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t id = 0, booted = 0;
    if ((err = rd(s, REG_MODEL_ID, &id, 1)) != ESP_OK || id != 0xEA) {
        ESP_LOGW(TAG, "0x%02X: model ID 0x%02X, not a VL53L1X", addr, id);
        vl53l1x_deinit(s);
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }
    for (int t = 0; t < 100 && !booted; t++) {
        rd(s, REG_FIRMWARE_SYSTEM_STATUS, &booted, 1);
        if (!booted) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (!booted) {
        vl53l1x_deinit(s);
        return ESP_ERR_TIMEOUT;
    }

    err = wr(s, 0x2D, DEFAULT_CONFIG, sizeof(DEFAULT_CONFIG));
    // One measurement completes the VHV calibration (ULD SensorInit).
    if (err == ESP_OK) err = wr8(s, REG_SYSTEM_MODE_START, 0x40);
    if (err == ESP_OK) err = wait_ready(s, 1000);
    if (err == ESP_OK) err = wr8(s, REG_SYSTEM_INTERRUPT_CLEAR, 0x01);
    if (err == ESP_OK) err = wr8(s, REG_SYSTEM_MODE_START, 0x00);
    if (err == ESP_OK) err = wr8(s, REG_VHV_TIMEOUT_LOOP, 0x09);
    if (err == ESP_OK) err = wr8(s, REG_VHV_START, 0x00);
    if (err == ESP_OK) err = set_mode(s, mode);
    if (err == ESP_OK) err = wr8(s, REG_SYSTEM_INTERRUPT_CLEAR, 0x01);
    if (err == ESP_OK) err = wr8(s, REG_SYSTEM_MODE_START, 0x40);   // continuous ranging
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X: init failed: %s", addr, esp_err_to_name(err));
        vl53l1x_deinit(s);
        return err;
    }
    ESP_LOGI(TAG, "0x%02X: ranging, %s mode", addr, mode == VL53L1X_SHORT ? "short" : "long");
    return ESP_OK;
}

esp_err_t vl53l1x_read(vl53l1x_t *s, uint16_t *mm, uint8_t *status)
{
    esp_err_t err = wait_ready(s, 600);   // the first measurement after start includes calibration
    uint8_t st = 0, d[2] = {0};
    if (err == ESP_OK) err = rd(s, REG_RESULT_RANGE_STATUS, &st, 1);
    if (err == ESP_OK) err = rd(s, REG_RESULT_DISTANCE, d, 2);
    if (err == ESP_OK) err = wr8(s, REG_SYSTEM_INTERRUPT_CLEAR, 0x01);
    if (err != ESP_OK) {
        return err;
    }
    st &= 0x1F;
    *status = st < sizeof(STATUS_MAP) ? STATUS_MAP[st] : 255;
    *mm = (uint16_t)(d[0] << 8 | d[1]);
    return ESP_OK;
}

void vl53l1x_deinit(vl53l1x_t *s)
{
    if (s->dev) {
        wr8(s, REG_SYSTEM_MODE_START, 0x00);
        i2c_master_bus_rm_device(s->dev);
        s->dev = NULL;
    }
}

const char *vl53l1x_status_text(uint8_t status)
{
    switch (status) {
    case 0: return "valid";
    case 1: return "sigma failure (noisy)";
    case 2: return "signal too weak (no target?)";
    case 4: return "out of range";
    case 7: return "wrap-around";
    default: return "invalid measurement";
    }
}
