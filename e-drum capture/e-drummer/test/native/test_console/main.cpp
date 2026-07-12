// Console command grammar.
#include <string.h>

#include <unity.h>

#include "edrum/console_cmd.h"

using namespace edrum;
using Kind = Command::Kind;

void setUp() {}
void tearDown() {}

static void test_simple_commands(void) {
    TEST_ASSERT_TRUE(parse_command("").kind == Kind::None);
    TEST_ASSERT_TRUE(parse_command("   ").kind == Kind::None);
    TEST_ASSERT_TRUE(parse_command("help").kind == Kind::Help);
    TEST_ASSERT_TRUE(parse_command("stats").kind == Kind::Stats);
    TEST_ASSERT_TRUE(parse_command("time").kind == Kind::Time);
    TEST_ASSERT_TRUE(parse_command("bookmark").kind == Kind::Bookmark);
    TEST_ASSERT_TRUE(parse_command("end").kind == Kind::EndSession);
    TEST_ASSERT_TRUE(parse_command("sync").kind == Kind::SyncEnter);
    TEST_ASSERT_TRUE(parse_command("bogus").kind == Kind::Invalid);
}

static void test_click(void) {
    Command c = parse_command("click 96");
    TEST_ASSERT_TRUE(c.kind == Kind::ClickStart);
    TEST_ASSERT_EQUAL_UINT16(96, c.bpm);
    TEST_ASSERT_EQUAL_UINT8(4, c.subdiv);

    c = parse_command("click 140 2");
    TEST_ASSERT_TRUE(c.kind == Kind::ClickStart);
    TEST_ASSERT_EQUAL_UINT8(2, c.subdiv);

    TEST_ASSERT_TRUE(parse_command("click off").kind == Kind::ClickStop);
    TEST_ASSERT_TRUE(parse_command("click").kind == Kind::Invalid);
    TEST_ASSERT_TRUE(parse_command("click 5000").kind == Kind::Invalid);
    TEST_ASSERT_TRUE(parse_command("click 120 99").kind == Kind::Invalid);
}

static void test_grid_enroll(void) {
    TEST_ASSERT_TRUE(parse_command("grid start").kind == Kind::GridStart);
    TEST_ASSERT_TRUE(parse_command("grid end").kind == Kind::GridEnd);
    TEST_ASSERT_TRUE(parse_command("grid").kind == Kind::Invalid);

    Command c = parse_command("enroll basic-rock");
    TEST_ASSERT_TRUE(c.kind == Kind::EnrollStart);
    TEST_ASSERT_EQUAL_STRING("basic-rock", c.ref);
    TEST_ASSERT_TRUE(parse_command("enroll end").kind == Kind::EnrollEnd);

    // bare `enroll`: zero-friction anonymous start, not an error (the whole
    // point — naming is never a precondition for starting a span)
    c = parse_command("enroll");
    TEST_ASSERT_TRUE(c.kind == Kind::EnrollStart);
    TEST_ASSERT_EQUAL_STRING("", c.ref);
}

static void test_settime(void) {
    Command c = parse_command("settime 2026-07-10T21:30:00+08:00");
    TEST_ASSERT_TRUE(c.kind == Kind::SetTime);
    TEST_ASSERT_EQUAL_INT(2026, c.dt.year);
    TEST_ASSERT_EQUAL_INT(480, c.dt.tz_offset_min);
    TEST_ASSERT_TRUE(parse_command("settime yesterday").kind == Kind::Invalid);
    TEST_ASSERT_TRUE(parse_command("settime").kind == Kind::Invalid);
}

static void test_burst(void) {
    Command c = parse_command("burst 2000");
    TEST_ASSERT_TRUE(c.kind == Kind::Burst);
    TEST_ASSERT_EQUAL_UINT32(2000, c.burst_count);
    TEST_ASSERT_EQUAL_UINT16(500, c.burst_hz);
    c = parse_command("burst 100 1000");
    TEST_ASSERT_EQUAL_UINT16(1000, c.burst_hz);
    TEST_ASSERT_TRUE(parse_command("burst").kind == Kind::Invalid);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_simple_commands);
    RUN_TEST(test_click);
    RUN_TEST(test_grid_enroll);
    RUN_TEST(test_settime);
    RUN_TEST(test_burst);
    return UNITY_END();
}
