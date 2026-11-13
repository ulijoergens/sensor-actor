// Unit tests for the WiFi reconnect schedule (REQ-WIFI-1).
#include "sdkconfig.h"
#include "unity.h"
#include "wifi_mgr.h"

#define S(x)   ((int64_t)(x) * 1000)
#define MIN(x) ((int64_t)(x) * 60 * 1000)
// Unity is built without 64-bit support; all values here fit in 32 bits.

static const wifi_timing_t fsd = {
    .fast_interval_ms = 10 * 1000, .fast_period_ms = 5 * 60 * 1000,
    .slow_interval_ms = 5 * 60 * 1000, .slow_period_ms = 60 * 60 * 1000,
    .ap_retry_ms = 15 * 60 * 1000,
};

TEST_CASE("every 10 s during the first 5 minutes", "[wifi]")
{
    TEST_ASSERT_EQUAL_INT32(S(10), wifi_next_retry_ms(0, &fsd));
    TEST_ASSERT_EQUAL_INT32(S(10), wifi_next_retry_ms(S(10), &fsd));
    TEST_ASSERT_EQUAL_INT32(S(10), wifi_next_retry_ms(MIN(5) - 1, &fsd));
}

TEST_CASE("every 5 min for the following hour", "[wifi]")
{
    TEST_ASSERT_EQUAL_INT32(MIN(5), wifi_next_retry_ms(MIN(5), &fsd));
    TEST_ASSERT_EQUAL_INT32(MIN(5), wifi_next_retry_ms(MIN(30), &fsd));
    TEST_ASSERT_EQUAL_INT32(MIN(5), wifi_next_retry_ms(MIN(65) - 1, &fsd));
}

TEST_CASE("access point after 5 min + 1 h", "[wifi]")
{
    TEST_ASSERT_EQUAL_INT32(-1, wifi_next_retry_ms(MIN(65), &fsd));
    TEST_ASSERT_EQUAL_INT32(-1, wifi_next_retry_ms(MIN(600), &fsd));
}

TEST_CASE("negative elapsed time is treated as 0", "[wifi]")
{
    TEST_ASSERT_EQUAL_INT32(S(10), wifi_next_retry_ms(-5, &fsd));
}

TEST_CASE("simulated outage: 30 fast + 12 slow attempts, then AP", "[wifi]")
{
    int64_t t = 0;
    int fast = 0, slow = 0;
    for (;;) {
        int64_t d = wifi_next_retry_ms(t, &fsd);
        if (d < 0) {
            break;
        }
        if (d == S(10)) fast++; else slow++;
        t += d;
    }
    TEST_ASSERT_EQUAL(30, fast);
    TEST_ASSERT_EQUAL(12, slow);
    TEST_ASSERT_EQUAL_INT32(MIN(65), t);
}

#ifndef CONFIG_SA_WIFI_TEST_TIMING
TEST_CASE("built-in timing matches the FSD", "[wifi]")
{
    const wifi_timing_t *t = wifi_mgr_timing();
    TEST_ASSERT_EQUAL(fsd.fast_interval_ms, t->fast_interval_ms);
    TEST_ASSERT_EQUAL(fsd.fast_period_ms, t->fast_period_ms);
    TEST_ASSERT_EQUAL(fsd.slow_interval_ms, t->slow_interval_ms);
    TEST_ASSERT_EQUAL(fsd.slow_period_ms, t->slow_period_ms);
    TEST_ASSERT_EQUAL(fsd.ap_retry_ms, t->ap_retry_ms);
}
#endif
