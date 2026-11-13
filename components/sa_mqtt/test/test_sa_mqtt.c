// Unit tests for MQTT topic building and command topic parsing.
#include <string.h>
#include "unity.h"
#include "sa_config.h"
#include "sa_mqtt.h"

TEST_CASE("topics are sensact/<id>/<sub>", "[mqtt]")
{
    char t[48];
    TEST_ASSERT_TRUE(sa_mqtt_topic(t, sizeof(t), "garden1", "status"));
    TEST_ASSERT_EQUAL_STRING("sensact/garden1/status", t);
    TEST_ASSERT_TRUE(sa_mqtt_topic(t, sizeof(t), "garden1", "sensor/tank/level"));
    TEST_ASSERT_EQUAL_STRING("sensact/garden1/sensor/tank/level", t);
    char small[10];
    TEST_ASSERT_FALSE(sa_mqtt_topic(small, sizeof(small), "garden1", "status"));
}

static bool parse(const char *topic, char *aid)
{
    return sa_mqtt_parse_actor_set(topic, strlen(topic), "garden1", aid, SA_ID_MAX + 1);
}

TEST_CASE("actor command topics are parsed and checked", "[mqtt]")
{
    char aid[SA_ID_MAX + 1];
    TEST_ASSERT_TRUE(parse("sensact/garden1/actor/pump/set", aid));
    TEST_ASSERT_EQUAL_STRING("pump", aid);
    TEST_ASSERT_TRUE(parse("sensact/garden1/actor/relay_01/set", aid));
    TEST_ASSERT_EQUAL_STRING("relay_01", aid);

    TEST_ASSERT_FALSE(parse("sensact/other/actor/pump/set", aid));       // other device
    TEST_ASSERT_FALSE(parse("sensact/garden1/actor/pump/state", aid));   // not a command
    TEST_ASSERT_FALSE(parse("sensact/garden1/actor//set", aid));         // empty ID
    TEST_ASSERT_FALSE(parse("sensact/garden1/actor/Pump/set", aid));     // invalid ID
    TEST_ASSERT_FALSE(parse("sensact/garden1/actor/toolongid/set", aid));
    TEST_ASSERT_FALSE(parse("sensact/garden1/actor/a/b/set", aid));      // nested
    TEST_ASSERT_FALSE(parse("sensact/garden1/actor/set", aid));
}

TEST_CASE("command topic without terminator (MQTT event buffers)", "[mqtt]")
{
    // MQTT event topics are not NUL-terminated; only topic_len counts.
    const char raw[] = "sensact/garden1/actor/pump/setGARBAGE";
    char aid[SA_ID_MAX + 1];
    TEST_ASSERT_TRUE(sa_mqtt_parse_actor_set(raw, strlen("sensact/garden1/actor/pump/set"), "garden1",
                                             aid, sizeof(aid)));
    TEST_ASSERT_EQUAL_STRING("pump", aid);
}
