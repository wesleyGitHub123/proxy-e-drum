// ISO formatting/parsing, epoch conversion, filename convention, uuid4 hex.
#include <string.h>

#include <unity.h>

#include "edrum/timefmt.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

static void test_format_iso(void) {
    DateTime dt;
    dt.year = 2026;
    dt.month = 7;
    dt.day = 6;
    dt.hour = 12;
    dt.minute = 0;
    dt.second = 0;
    dt.tz_offset_min = 480;
    char buf[32];
    TEST_ASSERT_EQUAL_UINT32(25, (uint32_t)format_iso(dt, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("2026-07-06T12:00:00+08:00", buf);

    dt.tz_offset_min = -330;  // -05:30
    format_iso(dt, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2026-07-06T12:00:00-05:30", buf);

    dt.tz_offset_min = 0;
    format_iso(dt, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2026-07-06T12:00:00+00:00", buf);
}

static void test_parse_iso_roundtrip(void) {
    DateTime dt;
    TEST_ASSERT_TRUE(parse_iso("2026-07-06T12:00:00+08:00", dt));
    TEST_ASSERT_EQUAL_INT(2026, dt.year);
    TEST_ASSERT_EQUAL_INT(7, dt.month);
    TEST_ASSERT_EQUAL_INT(6, dt.day);
    TEST_ASSERT_EQUAL_INT(480, dt.tz_offset_min);

    TEST_ASSERT_TRUE(parse_iso("2026-01-02T03:04:05Z", dt));
    TEST_ASSERT_EQUAL_INT(0, dt.tz_offset_min);
    TEST_ASSERT_TRUE(parse_iso("2026-01-02T03:04:05", dt));
    TEST_ASSERT_EQUAL_INT(0, dt.tz_offset_min);
    TEST_ASSERT_TRUE(parse_iso("2026-01-02T03:04:05-05:30", dt));
    TEST_ASSERT_EQUAL_INT(-330, dt.tz_offset_min);

    TEST_ASSERT_FALSE(parse_iso("2026-13-02T03:04:05", dt));   // bad month
    TEST_ASSERT_FALSE(parse_iso("2026-01-02 03:04:05", dt));   // no T
    TEST_ASSERT_FALSE(parse_iso("garbage", dt));
    TEST_ASSERT_FALSE(parse_iso("2026-01-02T03:04:05+8:00", dt));  // short offset
}

static void test_epoch_conversion(void) {
    DateTime dt;
    dt.year = 2026;
    dt.month = 7;
    dt.day = 6;
    dt.hour = 12;
    dt.minute = 0;
    dt.second = 0;
    dt.tz_offset_min = 480;
    const int64_t epoch = datetime_to_epoch(dt);  // 2026-07-06T04:00:00Z
    TEST_ASSERT_TRUE_MESSAGE(epoch == 1783310400LL, "epoch mismatch");  // 64-bit compare

    DateTime back;
    epoch_to_datetime(epoch, 480, back);
    TEST_ASSERT_EQUAL_INT(2026, back.year);
    TEST_ASSERT_EQUAL_INT(7, back.month);
    TEST_ASSERT_EQUAL_INT(6, back.day);
    TEST_ASSERT_EQUAL_INT(12, back.hour);
    TEST_ASSERT_EQUAL_INT(0, back.minute);

    epoch_to_datetime(0, 0, back);  // Unix epoch
    TEST_ASSERT_EQUAL_INT(1970, back.year);
    TEST_ASSERT_EQUAL_INT(1, back.month);
    TEST_ASSERT_EQUAL_INT(1, back.day);
}

static void test_session_filename(void) {
    DateTime dt;
    dt.year = 2026;
    dt.month = 7;
    dt.day = 7;
    dt.hour = 19;
    dt.minute = 19;
    dt.second = 44;
    char buf[48];
    const size_t n =
        session_filename(dt, "48e00a23ffffffffffffffffffffffff", buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("20260707T191944_48e00a23.jsonl", buf);
}

static void test_uuid4_hex(void) {
    uint8_t bytes[16] = {0};
    char out[33];
    uuid4_hex(bytes, out);
    TEST_ASSERT_EQUAL_UINT32(32, (uint32_t)strlen(out));
    TEST_ASSERT_EQUAL_CHAR('4', out[12]);  // version nibble
    TEST_ASSERT_EQUAL_CHAR('8', out[16]);  // variant nibble (0x00 -> 0x80)
    uint8_t ones[16];
    memset(ones, 0xFF, 16);
    uuid4_hex(ones, out);
    TEST_ASSERT_EQUAL_CHAR('4', out[12]);
    TEST_ASSERT_EQUAL_CHAR('b', out[16]);  // 0xFF -> 0xBF
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_format_iso);
    RUN_TEST(test_parse_iso_roundtrip);
    RUN_TEST(test_epoch_conversion);
    RUN_TEST(test_session_filename);
    RUN_TEST(test_uuid4_hex);
    return UNITY_END();
}
