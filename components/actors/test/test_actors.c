// Unit tests for the actors module's pure helpers.
#include <math.h>
#include <string.h>
#include "unity.h"
#include "actors.h"

static sa_device_cfg_t dev(void)
{
    sa_device_cfg_t d;
    const uint8_t mac[6] = {0, 0, 0, 1, 2, 3};
    sa_config_defaults(&d, mac);   // SDA 21, SCL 22, 1-Wire 4
    return d;
}

TEST_CASE("GPIO level follows state and active-low", "[actors]")
{
    TEST_ASSERT_EQUAL(1, sa_actor_gpio_level(true, false));
    TEST_ASSERT_EQUAL(0, sa_actor_gpio_level(false, false));
    TEST_ASSERT_EQUAL(0, sa_actor_gpio_level(true, true));
    TEST_ASSERT_EQUAL(1, sa_actor_gpio_level(false, true));
}

TEST_CASE("commands ON/OFF/TOGGLE", "[actors]")
{
    TEST_ASSERT_EQUAL(1, sa_actor_parse_command("ON"));
    TEST_ASSERT_EQUAL(1, sa_actor_parse_command(" on\n"));
    TEST_ASSERT_EQUAL(0, sa_actor_parse_command("OFF"));
    TEST_ASSERT_EQUAL(0, sa_actor_parse_command("0"));
    TEST_ASSERT_EQUAL(2, sa_actor_parse_command("toggle"));
    TEST_ASSERT_EQUAL(-1, sa_actor_parse_command(""));
    TEST_ASSERT_EQUAL(-1, sa_actor_parse_command("maybe"));
    TEST_ASSERT_EQUAL(-1, sa_actor_parse_command("ONONONONONONON"));
}

TEST_CASE("defaults are valid", "[actors]")
{
    sa_device_cfg_t d = dev();
    sa_actor_cfg_t a;
    sa_actor_defaults(&a, 26, 1);
    TEST_ASSERT_EQUAL_STRING("relay1", a.id);
    TEST_ASSERT_EQUAL_STRING("Relay 1 (GPIO 26)", a.label);
    TEST_ASSERT_TRUE(a.active);
    TEST_ASSERT_FALSE(a.rule_enabled);
    TEST_ASSERT_NULL(sa_actor_validate(&a, &d, NULL, 0, -1));
}

TEST_CASE("pins: flash, input-only, bus pins and duplicates are rejected", "[actors]")
{
    sa_device_cfg_t d = dev();
    sa_actor_cfg_t a, list[2];
    sa_actor_defaults(&a, 34, 1);
    TEST_ASSERT_NOT_NULL(sa_actor_validate(&a, &d, NULL, 0, -1));      // input-only
    a.gpio = 8;
    TEST_ASSERT_NOT_NULL(sa_actor_validate(&a, &d, NULL, 0, -1));      // flash
    a.gpio = 21;
    TEST_ASSERT_NOT_NULL(sa_actor_validate(&a, &d, NULL, 0, -1));      // I2C SDA
    a.gpio = 4;
    TEST_ASSERT_NOT_NULL(sa_actor_validate(&a, &d, NULL, 0, -1));      // 1-Wire
    a.gpio = 2;
    TEST_ASSERT_NULL(sa_actor_validate(&a, &d, NULL, 0, -1));          // strapping pin: allowed

    sa_actor_defaults(&list[0], 26, 1);
    sa_actor_defaults(&list[1], 27, 2);
    sa_actor_cfg_t b = list[1];
    b.gpio = 26;
    TEST_ASSERT_NOT_NULL(sa_actor_validate(&b, &d, list, 2, 1));       // pin used by relay1
    b.gpio = 27;
    TEST_ASSERT_NULL(sa_actor_validate(&b, &d, list, 2, 1));           // own entry is skipped
    strcpy(b.id, "relay1");
    TEST_ASSERT_NOT_NULL(sa_actor_validate(&b, &d, list, 2, 1));       // duplicate ID
}

TEST_CASE("rule settings are validated", "[actors]")
{
    sa_device_cfg_t d = dev();
    sa_actor_cfg_t a;
    sa_actor_defaults(&a, 26, 1);
    a.rule_enabled = true;
    TEST_ASSERT_NOT_NULL(sa_actor_validate(&a, &d, NULL, 0, -1));      // no sensor
    strcpy(a.rule_sensor, "tank");
    TEST_ASSERT_NOT_NULL(sa_actor_validate(&a, &d, NULL, 0, -1));      // no action
    a.rule.th[0] = (rule_threshold_t){.enabled = true, .value = 20, .falling = RULE_ACTION_OFF};
    TEST_ASSERT_NULL(sa_actor_validate(&a, &d, NULL, 0, -1));
    a.rule.th[1] = (rule_threshold_t){.enabled = true, .value = NAN, .rising = RULE_ACTION_ON};
    TEST_ASSERT_NOT_NULL(sa_actor_validate(&a, &d, NULL, 0, -1));      // NaN threshold
    a.rule.th[1].value = 30;
    TEST_ASSERT_NULL(sa_actor_validate(&a, &d, NULL, 0, -1));
    a.rule_quantity = SA_Q_COUNT;
    TEST_ASSERT_NOT_NULL(sa_actor_validate(&a, &d, NULL, 0, -1));
}
