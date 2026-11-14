// Unit tests for the captive-portal DNS reply builder.
#include <string.h>
#include "unity.h"
#include "sa_captive.h"

// Query for "connectivitycheck.gstatic.com", type A, class IN, RD set, ID 0x1234
static size_t make_query(uint8_t *q, uint16_t qtype)
{
    const uint8_t hdr[12] = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    const uint8_t name[] = "\x11" "connectivitycheck" "\x07" "gstatic" "\x03" "com";
    size_t n = 0;
    memcpy(q, hdr, 12); n = 12;
    memcpy(q + n, name, sizeof(name)); n += sizeof(name);          // includes the zero byte
    q[n++] = qtype >> 8; q[n++] = qtype & 0xFF; q[n++] = 0; q[n++] = 1;
    return n;
}

static const uint32_t AP_IP = 0x0104A8C0;   // 192.168.4.1 in network byte order

TEST_CASE("A query gets one A record with the AP address", "[captive]")
{
    uint8_t q[64], r[128];
    size_t qn = make_query(q, 1);
    size_t rn = sa_dns_reply(q, qn, r, sizeof(r), AP_IP);
    TEST_ASSERT_EQUAL(qn + 16, rn);
    TEST_ASSERT_EQUAL_HEX8(0x12, r[0]); TEST_ASSERT_EQUAL_HEX8(0x34, r[1]);   // same ID
    TEST_ASSERT_EQUAL_HEX8(0x85, r[2]);   // QR + AA + RD
    TEST_ASSERT_EQUAL_HEX8(0x00, r[3]);   // RCODE 0
    TEST_ASSERT_EQUAL(1, r[7]);           // ANCOUNT = 1
    TEST_ASSERT_EQUAL_MEMORY(q + 12, r + 12, qn - 12);   // question copied
    const uint8_t *a = r + qn;
    TEST_ASSERT_EQUAL_HEX8(0xC0, a[0]); TEST_ASSERT_EQUAL_HEX8(0x0C, a[1]);   // name pointer
    TEST_ASSERT_EQUAL(1, a[3]);           // TYPE A
    TEST_ASSERT_EQUAL(4, a[11]);          // RDLENGTH
    const uint8_t ip[4] = {192, 168, 4, 1};
    TEST_ASSERT_EQUAL_MEMORY(ip, a + 12, 4);
}

TEST_CASE("AAAA query gets an empty NOERROR answer", "[captive]")
{
    uint8_t q[64], r[128];
    size_t qn = make_query(q, 28);
    size_t rn = sa_dns_reply(q, qn, r, sizeof(r), AP_IP);
    TEST_ASSERT_EQUAL(qn, rn);
    TEST_ASSERT_EQUAL(0, r[7]);           // ANCOUNT = 0
    TEST_ASSERT_EQUAL_HEX8(0x00, r[3]);   // NOERROR
}

TEST_CASE("malformed and non-query packets are dropped", "[captive]")
{
    uint8_t q[64], r[128];
    size_t qn = make_query(q, 1);
    TEST_ASSERT_EQUAL(0, sa_dns_reply(q, 11, r, sizeof(r), AP_IP));        // too short
    TEST_ASSERT_EQUAL(0, sa_dns_reply(q, qn - 3, r, sizeof(r), AP_IP));    // truncated question
    uint8_t resp[64]; memcpy(resp, q, qn); resp[2] |= 0x80;                  // a response
    TEST_ASSERT_EQUAL(0, sa_dns_reply(resp, qn, r, sizeof(r), AP_IP));
    uint8_t noq[64]; memcpy(noq, q, qn); noq[5] = 0;                         // QDCOUNT 0
    TEST_ASSERT_EQUAL(0, sa_dns_reply(noq, qn, r, sizeof(r), AP_IP));
    uint8_t ptr[64]; memcpy(ptr, q, qn); ptr[12] = 0xC0;                     // compression in query
    TEST_ASSERT_EQUAL(0, sa_dns_reply(ptr, qn, r, sizeof(r), AP_IP));
    uint8_t lbl[64]; memcpy(lbl, q, qn); lbl[12] = 60;                       // label runs past the end
    TEST_ASSERT_EQUAL(0, sa_dns_reply(lbl, qn, r, sizeof(r), AP_IP));
    TEST_ASSERT_EQUAL(0, sa_dns_reply(q, qn, r, qn + 15, AP_IP));            // reply does not fit
}
