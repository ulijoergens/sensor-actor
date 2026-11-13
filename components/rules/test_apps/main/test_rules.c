// Unit tests for the rule engine (docs/FSD.md §4.1).
#include <string.h>
#include "unity.h"
#include "rules.h"

#define ON   RULE_ACTION_ON
#define OFF  RULE_ACTION_OFF
#define NONE RULE_ACTION_NONE

// Heater: OFF above 30, back ON at <= 28 (FSD §4.1 example 1).
static const rule_config_t heater = {
    .th = {
        {.enabled = true, .value = 28.0f, .rising = NONE, .falling = ON},
        {.enabled = true, .value = 30.0f, .rising = OFF, .falling = NONE},
    },
};

// Pump on water level (cm): OFF at <= 20, back ON above 30 (FSD §4.1 example 2).
static const rule_config_t pump = {
    .th = {
        {.enabled = true, .value = 20.0f, .rising = NONE, .falling = OFF},
        {.enabled = true, .value = 30.0f, .rising = ON, .falling = NONE},
    },
    .fault = OFF,
};

TEST_CASE("heater: no flicker between the thresholds", "[rules]")
{
    rule_state_t st = {0};
    TEST_ASSERT_EQUAL(NONE, rule_update(&heater, &st, 29.0f));  // first sample, between: keep
    TEST_ASSERT_EQUAL(OFF, rule_update(&heater, &st, 30.5f));   // crosses 30 upwards
    TEST_ASSERT_EQUAL(NONE, rule_update(&heater, &st, 29.5f));  // back below 30: nothing
    TEST_ASSERT_EQUAL(OFF, rule_update(&heater, &st, 30.2f));   // crosses 30 again: OFF again
    TEST_ASSERT_EQUAL(NONE, rule_update(&heater, &st, 28.5f));  // falls below 30, still above 28
    TEST_ASSERT_EQUAL(ON, rule_update(&heater, &st, 28.0f));    // reaches 28 (<= T): ON
    TEST_ASSERT_EQUAL(NONE, rule_update(&heater, &st, 27.0f));
    TEST_ASSERT_EQUAL(NONE, rule_update(&heater, &st, 29.9f));
}

TEST_CASE("heater: crossing 30 twice gives OFF each time", "[rules]")
{
    rule_state_t st = {0};
    rule_update(&heater, &st, 29.0f);
    TEST_ASSERT_EQUAL(OFF, rule_update(&heater, &st, 30.1f));
    TEST_ASSERT_EQUAL(NONE, rule_update(&heater, &st, 29.8f));
    TEST_ASSERT_EQUAL(OFF, rule_update(&heater, &st, 30.1f));
}

TEST_CASE("threshold boundary: v == T counts as <= T", "[rules]")
{
    rule_state_t st = {0};
    rule_update(&heater, &st, 31.0f);
    TEST_ASSERT_EQUAL(NONE, rule_update(&heater, &st, 30.0f));  // 30 is not > 30, falling none
    TEST_ASSERT_EQUAL(OFF, rule_update(&heater, &st, 30.01f));  // now above again
}

TEST_CASE("pump: dry-run protection and refill margin", "[rules]")
{
    rule_state_t st = {0};
    TEST_ASSERT_EQUAL(ON, rule_update(&pump, &st, 45.0f));      // first sample, full: ON
    TEST_ASSERT_EQUAL(NONE, rule_update(&pump, &st, 25.0f));
    TEST_ASSERT_EQUAL(OFF, rule_update(&pump, &st, 20.0f));     // reaches lower limit
    TEST_ASSERT_EQUAL(NONE, rule_update(&pump, &st, 26.0f));    // refilling, not enough yet
    TEST_ASSERT_EQUAL(NONE, rule_update(&pump, &st, 21.0f));    // dips, stays above 20
    TEST_ASSERT_EQUAL(ON, rule_update(&pump, &st, 31.0f));      // refilled with margin
    TEST_ASSERT_EQUAL(NONE, rule_update(&pump, &st, 22.0f));
    TEST_ASSERT_EQUAL(OFF, rule_update(&pump, &st, 19.0f));     // empty again
}

TEST_CASE("start-up: action for the side v is on (REQ-RUL-5)", "[rules]")
{
    rule_state_t st = {0};
    TEST_ASSERT_EQUAL(OFF, rule_update(&pump, &st, 10.0f));     // empty tank after power cut
    rule_state_reset(&st);
    TEST_ASSERT_EQUAL(NONE, rule_update(&pump, &st, 25.0f));    // between: power-on state
    rule_state_reset(&st);
    TEST_ASSERT_EQUAL(ON, rule_update(&heater, &st, 20.0f));
    rule_state_reset(&st);
    TEST_ASSERT_EQUAL(OFF, rule_update(&heater, &st, 35.0f));
}

TEST_CASE("one sample crossing both thresholds: last action wins (REQ-RUL-4)", "[rules]")
{
    const rule_config_t both = {
        .th = {
            {.enabled = true, .value = 10.0f, .rising = ON, .falling = OFF},
            {.enabled = true, .value = 20.0f, .rising = OFF, .falling = ON},
        },
    };
    rule_state_t st = {0};
    rule_update(&both, &st, 5.0f);
    TEST_ASSERT_EQUAL(OFF, rule_update(&both, &st, 25.0f));     // 10 rising ON, then 20 rising OFF
    TEST_ASSERT_EQUAL(OFF, rule_update(&both, &st, 5.0f));      // 20 falling ON, then 10 falling OFF
}

TEST_CASE("threshold order does not depend on T1/T2 slots", "[rules]")
{
    const rule_config_t swapped = {
        .th = {
            {.enabled = true, .value = 30.0f, .rising = OFF, .falling = NONE},
            {.enabled = true, .value = 28.0f, .rising = NONE, .falling = ON},
        },
    };
    rule_state_t a = {0}, b = {0};
    const float seq[] = {20, 29, 31, 29, 28, 35, 10};
    for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        TEST_ASSERT_EQUAL(rule_update(&heater, &a, seq[i]), rule_update(&swapped, &b, seq[i]));
    }
}

TEST_CASE("disabled threshold is ignored", "[rules]")
{
    rule_config_t one = heater;
    one.th[0].enabled = false;          // only the 30 / OFF threshold is left
    rule_state_t st = {0};
    TEST_ASSERT_EQUAL(NONE, rule_update(&one, &st, 20.0f));
    TEST_ASSERT_EQUAL(OFF, rule_update(&one, &st, 31.0f));
    TEST_ASSERT_EQUAL(NONE, rule_update(&one, &st, 20.0f));
}

TEST_CASE("fault action fires once, recovery acts like a first sample", "[rules]")
{
    rule_state_t st = {0};
    rule_update(&pump, &st, 25.0f);
    TEST_ASSERT_EQUAL(OFF, rule_fault(&pump, &st));             // sensor fails: pump OFF
    TEST_ASSERT_EQUAL(NONE, rule_fault(&pump, &st));            // still failing: nothing more
    TEST_ASSERT_EQUAL(ON, rule_update(&pump, &st, 40.0f));      // recovers full: ON
    TEST_ASSERT_EQUAL(OFF, rule_fault(&pump, &st));             // fails again: fires again
    TEST_ASSERT_EQUAL(NONE, rule_fault(&heater, &st));          // fault action none
}

TEST_CASE("config validation (REQ-RUL-7)", "[rules]")
{
    rule_config_t c = {0};
    TEST_ASSERT_FALSE(rule_config_valid(&c));
    c.th[1].enabled = true;
    TEST_ASSERT_FALSE(rule_config_valid(&c));
    c.th[1].falling = ON;
    TEST_ASSERT_TRUE(rule_config_valid(&c));
    c.th[1].enabled = false;
    TEST_ASSERT_FALSE(rule_config_valid(&c));
}

TEST_CASE("plain-language summary", "[rules]")
{
    char buf[160];
    rule_describe(&heater, "temperature", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("ON when temperature falls to 28 or below; OFF when temperature rises above 30", buf);
    rule_describe(&pump, "level", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("OFF when level falls to 20 or below; ON when level rises above 30", buf);

    rule_config_t empty = {0};
    rule_describe(&empty, "x", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("no action", buf);

    char small[12];
    int n = rule_describe(&heater, "temperature", small, sizeof(small));
    TEST_ASSERT_EQUAL(11, n);
    TEST_ASSERT_EQUAL(11, (int)strlen(small));
}
