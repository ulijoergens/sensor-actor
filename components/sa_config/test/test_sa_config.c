// Unit tests for settings defaults and validation.
#include <string.h>
#include "unity.h"
#include "sa_config.h"

static const uint8_t MAC[6] = {0x24, 0x6f, 0x28, 0xa1, 0xb2, 0xc3};

static sa_device_cfg_t good(void)
{
    sa_device_cfg_t c;
    sa_config_defaults(&c, MAC);
    return c;
}

TEST_CASE("defaults are derived from the MAC and are valid", "[config]")
{
    sa_device_cfg_t c = good();
    TEST_ASSERT_EQUAL_STRING("SensAct-B2C3", c.name);
    TEST_ASSERT_EQUAL_STRING("saa1b2c3", c.id);
    TEST_ASSERT_EQUAL(1883, c.mqtt_port);
    TEST_ASSERT_TRUE(c.discovery);
    TEST_ASSERT_EQUAL_STRING("homeassistant", c.discovery_prefix);
    TEST_ASSERT_EQUAL(21, c.i2c_sda);
    TEST_ASSERT_EQUAL(22, c.i2c_scl);
    TEST_ASSERT_EQUAL(4, c.onewire);
    TEST_ASSERT_NULL(sa_config_validate(&c));
}

TEST_CASE("short IDs: 1-8 chars of a-z 0-9 _ -", "[config]")
{
    TEST_ASSERT_TRUE(sa_id_valid("a"));
    TEST_ASSERT_TRUE(sa_id_valid("tank_01"));
    TEST_ASSERT_TRUE(sa_id_valid("pump-2"));
    TEST_ASSERT_TRUE(sa_id_valid("12345678"));
    TEST_ASSERT_FALSE(sa_id_valid(""));
    TEST_ASSERT_FALSE(sa_id_valid("123456789"));
    TEST_ASSERT_FALSE(sa_id_valid("Tank"));
    TEST_ASSERT_FALSE(sa_id_valid("a b"));
    TEST_ASSERT_FALSE(sa_id_valid("a/b"));
    TEST_ASSERT_FALSE(sa_id_valid("a+"));
}

TEST_CASE("GPIO classification for the ESP32", "[config]")
{
    TEST_ASSERT_TRUE(sa_gpio_output_ok(4));
    TEST_ASSERT_TRUE(sa_gpio_output_ok(21));
    TEST_ASSERT_TRUE(sa_gpio_output_ok(33));
    TEST_ASSERT_FALSE(sa_gpio_output_ok(6));    // flash
    TEST_ASSERT_FALSE(sa_gpio_output_ok(11));   // flash
    TEST_ASSERT_FALSE(sa_gpio_output_ok(20));   // not bonded out
    TEST_ASSERT_FALSE(sa_gpio_output_ok(24));
    TEST_ASSERT_FALSE(sa_gpio_output_ok(34));   // input-only
    TEST_ASSERT_FALSE(sa_gpio_output_ok(39));
    TEST_ASSERT_FALSE(sa_gpio_output_ok(40));
    TEST_ASSERT_FALSE(sa_gpio_output_ok(-1));
    TEST_ASSERT_TRUE(sa_gpio_exists(36));
    TEST_ASSERT_TRUE(sa_gpio_is_strapping(12));
    TEST_ASSERT_FALSE(sa_gpio_is_strapping(4));
}

TEST_CASE("validation rejects bad fields with a message", "[config]")
{
    sa_device_cfg_t c;

    c = good(); c.name[0] = '\0';
    TEST_ASSERT_NOT_NULL(sa_config_validate(&c));
    c = good(); strcpy(c.name, "Bad\tname");
    TEST_ASSERT_NOT_NULL(sa_config_validate(&c));
    c = good(); strcpy(c.id, "BAD");
    TEST_ASSERT_NOT_NULL(sa_config_validate(&c));

    c = good(); strcpy(c.wifi_ssid, "home"); strcpy(c.wifi_pass, "short");
    TEST_ASSERT_NOT_NULL(sa_config_validate(&c));
    strcpy(c.wifi_pass, "longenough");
    TEST_ASSERT_NULL(sa_config_validate(&c));
    c.wifi_pass[0] = '\0';                                   // open network
    TEST_ASSERT_NULL(sa_config_validate(&c));

    c = good(); strcpy(c.mqtt_host, "192.168.1.10");
    TEST_ASSERT_NULL(sa_config_validate(&c));
    strcpy(c.mqtt_host, "broker.home.local");
    TEST_ASSERT_NULL(sa_config_validate(&c));
    strcpy(c.mqtt_host, "bad host");
    TEST_ASSERT_NOT_NULL(sa_config_validate(&c));
    strcpy(c.mqtt_host, "broker"); c.mqtt_port = 0;
    TEST_ASSERT_NOT_NULL(sa_config_validate(&c));
    c.mqtt_host[0] = '\0';                                   // MQTT off: port not checked
    TEST_ASSERT_NULL(sa_config_validate(&c));

    c = good(); strcpy(c.discovery_prefix, "ha/#");
    TEST_ASSERT_NOT_NULL(sa_config_validate(&c));
    strcpy(c.discovery_prefix, "/ha");
    TEST_ASSERT_NOT_NULL(sa_config_validate(&c));
    c.discovery = false;                                     // prefix ignored when off
    TEST_ASSERT_NULL(sa_config_validate(&c));
}

TEST_CASE("validation rejects unusable and duplicate pins", "[config]")
{
    sa_device_cfg_t c;
    c = good(); c.i2c_sda = 34;                              // input-only
    TEST_ASSERT_EQUAL_STRING("I2C SDA pin: GPIO 34 cannot be used", sa_config_validate(&c));
    c = good(); c.onewire = 7;                               // flash
    TEST_ASSERT_NOT_NULL(sa_config_validate(&c));
    c = good(); c.onewire = 21;                              // same as SDA
    TEST_ASSERT_EQUAL_STRING("1-Wire pin: GPIO 21 is already used", sa_config_validate(&c));
    c = good(); c.i2c_scl = c.i2c_sda;
    TEST_ASSERT_NOT_NULL(sa_config_validate(&c));
}
