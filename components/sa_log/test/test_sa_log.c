// Unit tests for log line cleaning.
#include "unity.h"
#include "sa_log.h"

TEST_CASE("log lines lose prefix, colour codes and line ends", "[log]")
{
    char out[64];
    sa_log_clean_line(out, sizeof(out), "\033[0;33mW (12345) wifi: connection lost (reason 2)\033[0m\n");
    TEST_ASSERT_EQUAL_STRING("wifi: connection lost (reason 2)", out);
    sa_log_clean_line(out, sizeof(out), "E (7) crash: boom\r\n");
    TEST_ASSERT_EQUAL_STRING("crash: boom", out);
    sa_log_clean_line(out, sizeof(out), "plain text");
    TEST_ASSERT_EQUAL_STRING("plain text", out);
    char small[8];
    sa_log_clean_line(small, sizeof(small), "W (1) abcdefghijkl");
    TEST_ASSERT_EQUAL_STRING("abcdefg", small);
}
