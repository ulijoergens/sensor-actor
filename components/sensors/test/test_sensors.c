// Unit tests for the sensor framework's pure helpers.
#include <math.h>
#include <string.h>
#include "unity.h"
#include "sensors.h"

TEST_CASE("median of odd and even counts, input unchanged", "[sensors]")
{
    float a[] = {5, 1, 4, 2, 3};
    TEST_ASSERT_EQUAL_FLOAT(3, sa_median(a, 5));
    TEST_ASSERT_EQUAL_FLOAT(5, a[0]);                 // not sorted in place
    float b[] = {10, 2, 8, 4};
    TEST_ASSERT_EQUAL_FLOAT(6, sa_median(b, 4));      // (4 + 8) / 2
    float c[] = {7};
    TEST_ASSERT_EQUAL_FLOAT(7, sa_median(c, 1));
    TEST_ASSERT_TRUE(isnan(sa_median(c, 0)));
}

TEST_CASE("median ignores a single outlier", "[sensors]")
{
    float v[] = {1260, 1258, 1261, 40, 1259};         // hand passing in front of the sensor
    TEST_ASSERT_EQUAL_FLOAT(1259, sa_median(v, 5));
}

TEST_CASE("quantities per chip; level only with mounting height", "[sensors]")
{
    sa_sensor_cfg_t c;
    sa_quantity_t q[SA_MAX_QUANTITIES];
    sa_sensor_defaults(&c, SA_CHIP_VL53L1X, 0x29, 0, 1);
    TEST_ASSERT_EQUAL(1, sa_sensor_quantities(&c, q));
    TEST_ASSERT_EQUAL(SA_Q_DISTANCE, q[0]);
    c.mount_mm = 2000;
    TEST_ASSERT_EQUAL(2, sa_sensor_quantities(&c, q));
    TEST_ASSERT_EQUAL(SA_Q_LEVEL, q[1]);
    sa_sensor_defaults(&c, SA_CHIP_BME280, 0x76, 0, 1);
    TEST_ASSERT_EQUAL(3, sa_sensor_quantities(&c, q));
    sa_sensor_defaults(&c, SA_CHIP_SHT3X, 0x44, 0, 1);
    TEST_ASSERT_EQUAL(2, sa_sensor_quantities(&c, q));
    TEST_ASSERT_EQUAL(SA_Q_HUMIDITY, q[1]);
}

TEST_CASE("defaults: ID by type, valid settings", "[sensors]")
{
    sa_sensor_cfg_t c;
    sa_sensor_defaults(&c, SA_CHIP_VL53L1X, 0x29, 0, 1);
    TEST_ASSERT_EQUAL_STRING("dist1", c.id);
    TEST_ASSERT_EQUAL_STRING("VL53L1X 0x29", c.label);
    TEST_ASSERT_TRUE(c.active);
    TEST_ASSERT_EQUAL(10, c.interval_s);
    TEST_ASSERT_EQUAL(15, c.median_n);
    TEST_ASSERT_NULL(sa_sensor_validate(&c));
    sa_sensor_defaults(&c, SA_CHIP_AHT20, 0x38, 0, 2);
    TEST_ASSERT_EQUAL_STRING("th2", c.id);
    sa_sensor_defaults(&c, SA_CHIP_BME280, 0x76, 0, 3);
    TEST_ASSERT_EQUAL_STRING("env3", c.id);
}

TEST_CASE("validation of sensor settings", "[sensors]")
{
    sa_sensor_cfg_t c;
    sa_sensor_defaults(&c, SA_CHIP_VL53L1X, 0x29, 0, 1);
    sa_sensor_cfg_t t;
    t = c; strcpy(t.id, "Dist");     TEST_ASSERT_NOT_NULL(sa_sensor_validate(&t));
    t = c; t.label[0] = '\0';        TEST_ASSERT_NOT_NULL(sa_sensor_validate(&t));
    t = c; t.interval_s = 0;         TEST_ASSERT_NOT_NULL(sa_sensor_validate(&t));
    t = c; t.interval_s = 3601;      TEST_ASSERT_NOT_NULL(sa_sensor_validate(&t));
    t = c; t.median_n = 2;           TEST_ASSERT_NOT_NULL(sa_sensor_validate(&t));
    t = c; t.median_n = 32;          TEST_ASSERT_NOT_NULL(sa_sensor_validate(&t));
    t = c; t.vl_mode = 3;            TEST_ASSERT_NOT_NULL(sa_sensor_validate(&t));
    t = c; t.mount_mm = 4001;        TEST_ASSERT_NOT_NULL(sa_sensor_validate(&t));
    t = c; t.offset[SA_Q_DISTANCE] = NAN; TEST_ASSERT_NOT_NULL(sa_sensor_validate(&t));
    t = c; strcpy(t.label, "Tank (rain water)"); TEST_ASSERT_NULL(sa_sensor_validate(&t));
}

TEST_CASE("quantity names and units", "[sensors]")
{
    TEST_ASSERT_EQUAL_STRING("level", sa_quantity_name(SA_Q_LEVEL));
    TEST_ASSERT_EQUAL_STRING("mm", sa_quantity_unit(SA_Q_DISTANCE));
    TEST_ASSERT_EQUAL_STRING("hPa", sa_quantity_unit(SA_Q_PRESSURE));
}

TEST_CASE("1-Wire ROM codes: family code first, round trip", "[sensors]")
{
    uint64_t rom = 0;
    TEST_ASSERT_TRUE(sa_rom_from_str("28FFB8F2801704CE", &rom));
    TEST_ASSERT_EQUAL_HEX8(0x28, (uint8_t)(rom & 0xFF));          // family code in byte 0
    TEST_ASSERT_EQUAL_HEX8(0xCE, (uint8_t)(rom >> 56));            // CRC in byte 7
    char s[17];
    sa_rom_to_str(rom, s);
    TEST_ASSERT_EQUAL_STRING("28FFB8F2801704CE", s);
    TEST_ASSERT_TRUE(sa_rom_from_str("28ffb8f2801704ce", &rom));  // lower case accepted
    TEST_ASSERT_FALSE(sa_rom_from_str("28FFB8F2801704C", &rom));   // too short
    TEST_ASSERT_FALSE(sa_rom_from_str("28FFB8F2801704CEX", &rom)); // too long
    TEST_ASSERT_FALSE(sa_rom_from_str("28FFB8F2801704XX", &rom));  // not hex
}

TEST_CASE("DS18B20 defaults and validation", "[sensors]")
{
    uint64_t rom;
    sa_rom_from_str("28FFB8F2801704CE", &rom);
    sa_sensor_cfg_t c;
    sa_sensor_defaults(&c, SA_CHIP_DS18B20, 0, rom, 1);
    TEST_ASSERT_EQUAL_STRING("temp1", c.id);
    TEST_ASSERT_EQUAL_STRING("DS18B20 1704CE", c.label);
    TEST_ASSERT_EQUAL(12, c.ds_resolution);
    TEST_ASSERT_NULL(sa_sensor_validate(&c));
    sa_quantity_t q[SA_MAX_QUANTITIES];
    TEST_ASSERT_EQUAL(1, sa_sensor_quantities(&c, q));
    TEST_ASSERT_EQUAL(SA_Q_TEMPERATURE, q[0]);
    sa_sensor_cfg_t t = c; t.ds_resolution = 8;   TEST_ASSERT_NOT_NULL(sa_sensor_validate(&t));
    t = c; t.ds_resolution = 13;                   TEST_ASSERT_NOT_NULL(sa_sensor_validate(&t));
    t = c; t.rom = (rom & ~0xFFULL) | 0x10;        TEST_ASSERT_NOT_NULL(sa_sensor_validate(&t));  // DS18S20 family
}
