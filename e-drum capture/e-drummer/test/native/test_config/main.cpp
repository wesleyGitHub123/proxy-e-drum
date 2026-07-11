// key=value config parsing: defaults, application, validation, tolerance.
#include <string.h>

#include <unity.h>

#include "edrum/config.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

static void test_defaults(void) {
    Config c;
    TEST_ASSERT_EQUAL_STRING("local", c.user_id);
    TEST_ASSERT_EQUAL_STRING("td02k", c.kit_profile_id);
    TEST_ASSERT_FALSE(c.has_calibration);
    TEST_ASSERT_EQUAL_INT(480, c.tz_offset_min);
    TEST_ASSERT_EQUAL_UINT16(120, c.click_bpm);
    TEST_ASSERT_EQUAL_UINT32(3000, c.pause_after_ms);
    TEST_ASSERT_EQUAL_UINT32(300000, c.idle_end_ms);
    TEST_ASSERT_TRUE(c.gesture_enable);
    TEST_ASSERT_EQUAL_UINT8(36, c.gesture_note_a);
    TEST_ASSERT_EQUAL_UINT8(49, c.gesture_note_b);
}

static void test_parse_full_file(void) {
    const char* text =
        "# e-drummer config\n"
        "user_id = wesley\n"
        "kit_profile_id = td02k\n"
        "calibration_offset_ms = -12\n"
        "tz_offset_min = 480\n"
        "click_bpm = 96\n"
        "click_subdiv = 2\n"
        "pause_after_ms = 4000\n"
        "idle_end_ms = 120000\n"
        "gesture_enable = off\n"
        "\n";
    Config c;
    const ConfigParseStats st = parse_config(text, strlen(text), c);
    TEST_ASSERT_EQUAL_UINT16(9, st.applied);
    TEST_ASSERT_EQUAL_UINT16(0, st.unknown);
    TEST_ASSERT_EQUAL_UINT16(0, st.invalid);
    TEST_ASSERT_EQUAL_STRING("wesley", c.user_id);
    TEST_ASSERT_TRUE(c.has_calibration);
    TEST_ASSERT_EQUAL_INT32(-12, c.calibration_offset_ms);
    TEST_ASSERT_EQUAL_UINT16(96, c.click_bpm);
    TEST_ASSERT_EQUAL_UINT8(2, c.click_subdiv);
    TEST_ASSERT_EQUAL_UINT32(4000, c.pause_after_ms);
    TEST_ASSERT_FALSE(c.gesture_enable);
}

static void test_null_values(void) {
    const char* text = "kit_profile_id = null\ncalibration_offset_ms = null\n";
    Config c;
    c.has_calibration = true;
    parse_config(text, strlen(text), c);
    TEST_ASSERT_EQUAL_STRING("", c.kit_profile_id);
    TEST_ASSERT_FALSE(c.has_calibration);
}

static void test_invalid_and_unknown(void) {
    const char* text =
        "click_bpm = 9000\n"       // out of range
        "user_id = has\"quote\n"   // forbidden char
        "user_id =\n"              // empty
        "no_equals_line\n"         // malformed
        "future_key = 42\n";       // unknown: tolerated
    Config c;
    const ConfigParseStats st = parse_config(text, strlen(text), c);
    TEST_ASSERT_EQUAL_UINT16(0, st.applied);
    TEST_ASSERT_EQUAL_UINT16(1, st.unknown);
    TEST_ASSERT_EQUAL_UINT16(4, st.invalid);
    TEST_ASSERT_EQUAL_STRING("local", c.user_id);       // untouched
    TEST_ASSERT_EQUAL_UINT16(120, c.click_bpm);         // untouched
}

static void test_crlf_and_spacing_tolerance(void) {
    const char* text = "  click_bpm=140  \r\n\tuser_id\t=\tw\r\n";
    Config c;
    const ConfigParseStats st = parse_config(text, strlen(text), c);
    TEST_ASSERT_EQUAL_UINT16(2, st.applied);
    TEST_ASSERT_EQUAL_UINT16(140, c.click_bpm);
    TEST_ASSERT_EQUAL_STRING("w", c.user_id);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_parse_full_file);
    RUN_TEST(test_null_values);
    RUN_TEST(test_invalid_and_unknown);
    RUN_TEST(test_crlf_and_spacing_tolerance);
    return UNITY_END();
}
