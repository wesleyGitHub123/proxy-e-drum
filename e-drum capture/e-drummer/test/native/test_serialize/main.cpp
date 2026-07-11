// Canonical serialization unit tests — every record type against exact
// expected bytes (micro-decision 6 key orders), plus escaping edge cases.
#include <string.h>

#include <unity.h>

#include "edrum/serialize.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

static char buf[kMaxLine];

static const char* line_of(const CaptureRecord& r) {
    const size_t n = record_line(r, buf, sizeof(buf));
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "record_line failed");
    buf[n] = '\0';
    return buf;
}

static void test_event_line(void) {
    CaptureRecord r{};
    r.type = RecType::Event;
    r.t = 100;
    r.u.event = EventP{36, 96, 9};
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"event\",\"t\":100,\"note\":36,\"velocity\":96,\"channel\":9}\n",
                             line_of(r));
}

static void test_ctrl_control_change(void) {
    CaptureRecord r{};
    r.type = RecType::CtrlMidi;
    r.t = 50;
    r.u.ctrl.msg = MidiMsg{0xB9, 4, 88};
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ctrl\",\"t\":50,\"msg\":{\"type\":\"control_change\",\"channel\":9,\"control\":4,\"value\":88}}\n",
        line_of(r));
}

static void test_ctrl_note_off(void) {
    CaptureRecord r{};
    r.type = RecType::CtrlMidi;
    r.t = 6200;
    r.u.ctrl.msg = MidiMsg{0x89, 42, 64};
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ctrl\",\"t\":6200,\"msg\":{\"type\":\"note_off\",\"channel\":9,\"note\":42,\"velocity\":64}}\n",
        line_of(r));
}

static void test_ctrl_note_on_velocity_zero(void) {
    // micro-decision 3: note_on v=0 is a ctrl record, type stays "note_on"
    CaptureRecord r{};
    r.type = RecType::CtrlMidi;
    r.t = 7;
    r.u.ctrl.msg = MidiMsg{0x99, 38, 0};
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ctrl\",\"t\":7,\"msg\":{\"type\":\"note_on\",\"channel\":9,\"note\":38,\"velocity\":0}}\n",
        line_of(r));
}

static void test_ctrl_pitchwheel_negative(void) {
    CaptureRecord r{};
    r.type = RecType::CtrlMidi;
    r.t = 1;
    r.u.ctrl.msg = MidiMsg{0xE5, 0, 0};  // 14-bit 0 -> -8192
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ctrl\",\"t\":1,\"msg\":{\"type\":\"pitchwheel\",\"channel\":5,\"pitch\":-8192}}\n",
        line_of(r));
    r.u.ctrl.msg = MidiMsg{0xE5, 0x7F, 0x7F};  // max -> 8191
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ctrl\",\"t\":1,\"msg\":{\"type\":\"pitchwheel\",\"channel\":5,\"pitch\":8191}}\n",
        line_of(r));
    r.u.ctrl.msg = MidiMsg{0xE5, 0, 0x40};  // center -> 0
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ctrl\",\"t\":1,\"msg\":{\"type\":\"pitchwheel\",\"channel\":5,\"pitch\":0}}\n",
        line_of(r));
}

static void test_ctrl_aftertouch_and_program(void) {
    CaptureRecord r{};
    r.type = RecType::CtrlMidi;
    r.t = 2;
    r.u.ctrl.msg = MidiMsg{0xD9, 77, 0};
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ctrl\",\"t\":2,\"msg\":{\"type\":\"aftertouch\",\"channel\":9,\"value\":77}}\n",
        line_of(r));
    r.u.ctrl.msg = MidiMsg{0xC0, 5, 0};
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ctrl\",\"t\":2,\"msg\":{\"type\":\"program_change\",\"channel\":0,\"program\":5}}\n",
        line_of(r));
    r.u.ctrl.msg = MidiMsg{0xA9, 42, 15};
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ctrl\",\"t\":2,\"msg\":{\"type\":\"polytouch\",\"channel\":9,\"note\":42,\"value\":15}}\n",
        line_of(r));
}

static void test_ctrl_sysex(void) {
    CaptureRecord r{};
    r.type = RecType::CtrlSysex;
    r.t = 30;
    r.u.sysex.len = 4;
    r.u.sysex.truncated = 0;
    const uint8_t d[4] = {65, 16, 0, 17};
    memcpy(r.u.sysex.data, d, 4);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ctrl\",\"t\":30,\"msg\":{\"type\":\"sysex\",\"data\":[65,16,0,17]}}\n", line_of(r));
    r.u.sysex.len = 0;
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ctrl\",\"t\":30,\"msg\":{\"type\":\"sysex\",\"data\":[]}}\n", line_of(r));
}

static void test_declarations(void) {
    CaptureRecord r{};
    r.type = RecType::GridStart;
    r.t = 1000;
    r.u.grid = GridP{120, 4, 1000};
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"grid_start\",\"t\":1000,\"bpm\":120,\"subdiv\":4,\"downbeat_t\":1000}\n",
        line_of(r));

    r.type = RecType::GridEnd;
    r.t = 3000;
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"grid_end\",\"t\":3000}\n", line_of(r));

    r.type = RecType::Bookmark;
    r.t = 3500;
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"bookmark\",\"t\":3500}\n", line_of(r));

    r.type = RecType::EnrollStart;
    r.t = 4000;
    r.u.enroll = EnrollP{};
    strcpy(r.u.enroll.profile_ref, "basic-rock");
    r.u.enroll.bpm = 120;
    r.u.enroll.subdiv = 4;
    r.u.enroll.downbeat_t = 4000;
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"enroll_start\",\"t\":4000,\"profile_ref\":\"basic-rock\",\"bpm\":120,\"subdiv\":4,\"downbeat_t\":4000}\n",
        line_of(r));

    r.type = RecType::EnrollEnd;
    r.t = 6000;
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"enroll_end\",\"t\":6000}\n", line_of(r));

    r.type = RecType::SessionEnd;
    r.t = 6500;
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"session_end\",\"t\":6500}\n", line_of(r));
}

static void test_meta_null_and_int_calibration(void) {
    SessionStartP s{};
    strcpy(s.session_id, "11111111aaaa4bbb8ccc000000000001");
    strcpy(s.start_iso, "2026-07-06T12:00:00+08:00");
    MetaStatic ms{};
    strcpy(ms.kit_profile_id, "td02k");
    strcpy(ms.user_id, "local");
    ms.has_calibration = false;

    size_t n = meta_line(s, ms, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"meta\",\"schema_version\":1,\"session_id\":\"11111111aaaa4bbb8ccc000000000001\","
        "\"start_iso\":\"2026-07-06T12:00:00+08:00\",\"kit_profile_id\":\"td02k\",\"user_id\":\"local\","
        "\"calibration_offset_ms\":null}\n",
        buf);

    ms.has_calibration = true;
    ms.calibration_offset_ms = -12;
    n = meta_line(s, ms, buf, sizeof(buf));
    buf[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"calibration_offset_ms\":-12}"));

    ms.kit_profile_id[0] = '\0';  // null kit profile
    n = meta_line(s, ms, buf, sizeof(buf));
    buf[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"kit_profile_id\":null,"));
}

static void test_json_escape(void) {
    char out[64];
    size_t n = json_escape("a\"b\\c", out, sizeof(out));
    out[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\"", out);
    n = json_escape("x\ty\nz", out, sizeof(out));
    out[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("\"x\\ty\\nz\"", out);
    char in[3] = {(char)0x1B, 'q', 0};
    n = json_escape(in, out, sizeof(out));
    out[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("\"\\u001bq\"", out);
}

static void test_overflow_returns_zero(void) {
    CaptureRecord r{};
    r.type = RecType::Event;
    r.t = 100;
    r.u.event = EventP{36, 96, 9};
    char tiny[16];
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)record_line(r, tiny, sizeof(tiny)));
    // SessionStart is a marker, never a line
    r.type = RecType::SessionStart;
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)record_line(r, buf, sizeof(buf)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_event_line);
    RUN_TEST(test_ctrl_control_change);
    RUN_TEST(test_ctrl_note_off);
    RUN_TEST(test_ctrl_note_on_velocity_zero);
    RUN_TEST(test_ctrl_pitchwheel_negative);
    RUN_TEST(test_ctrl_aftertouch_and_program);
    RUN_TEST(test_ctrl_sysex);
    RUN_TEST(test_declarations);
    RUN_TEST(test_meta_null_and_int_calibration);
    RUN_TEST(test_json_escape);
    RUN_TEST(test_overflow_returns_zero);
    return UNITY_END();
}
