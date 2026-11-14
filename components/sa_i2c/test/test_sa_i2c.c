// Unit tests for I2C helpers.
#include "unity.h"
#include "sa_i2c.h"

TEST_CASE("Sensirion CRC-8 matches the datasheet example", "[i2c]")
{
    const uint8_t beef[2] = {0xBE, 0xEF};          // SHT3x datasheet: CRC(0xBEEF) = 0x92
    TEST_ASSERT_EQUAL_HEX8(0x92, sa_crc8_sensirion(beef, 2));
    const uint8_t zero[2] = {0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX8(0x81, sa_crc8_sensirion(zero, 2));
}

TEST_CASE("chip names", "[i2c]")
{
    TEST_ASSERT_EQUAL_STRING("VL53L1X", sa_chip_name(SA_CHIP_VL53L1X));
    TEST_ASSERT_EQUAL_STRING("BME280", sa_chip_name(SA_CHIP_BME280));
    TEST_ASSERT_EQUAL_STRING("unknown device", sa_chip_name(SA_CHIP_UNKNOWN));
}
