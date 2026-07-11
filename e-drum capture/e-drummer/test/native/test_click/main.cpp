// Click schedule arithmetic (no drift, exact anchors) and block rendering
// (edges land at the right sample offsets; lateness is counted, not lost).
#include <unity.h>

#include "edrum/click_render.h"
#include "edrum/click_sched.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

static void test_beat_arithmetic_no_drift(void) {
    ClickScheduler s;
    s.start(1000000, 120, 4);  // 500 ms period
    TEST_ASSERT_TRUE(s.beat_us(0) == 1000000ULL);
    TEST_ASSERT_TRUE(s.beat_us(1) == 1500000ULL);
    TEST_ASSERT_TRUE(s.beat_us(120) == 61000000ULL);  // exactly 60 s later

    // 90 bpm: 666666.67 µs period — rounding never accumulates
    s.start(0, 90, 4);
    TEST_ASSERT_TRUE(s.beat_us(1) == 666667ULL);
    TEST_ASSERT_TRUE(s.beat_us(3) == 2000000ULL);   // 3 beats = exactly 2 s
    TEST_ASSERT_TRUE(s.beat_us(90) == 60000000ULL); // 90 beats = exactly 60 s
}

static void test_accent_every_bar(void) {
    ClickScheduler s;
    s.start(0, 120, 4, 4);
    TEST_ASSERT_TRUE(s.next_is_accent());  // beat 0
    s.advance();
    TEST_ASSERT_FALSE(s.next_is_accent());
    s.advance();
    s.advance();
    s.advance();
    TEST_ASSERT_TRUE(s.next_is_accent());  // beat 4
}

static void test_next_edge_at_or_after(void) {
    ClickScheduler s;
    s.start(1000000, 120, 4);
    TEST_ASSERT_TRUE(s.next_edge_at_or_after(0) == 1000000ULL);
    TEST_ASSERT_TRUE(s.next_edge_at_or_after(1000000) == 1000000ULL);
    TEST_ASSERT_TRUE(s.next_edge_at_or_after(1000001) == 1500000ULL);
    TEST_ASSERT_TRUE(s.next_edge_at_or_after(1499999) == 1500000ULL);
    TEST_ASSERT_TRUE(s.next_edge_at_or_after(1500000) == 1500000ULL);
}

static void test_render_places_click_at_offset(void) {
    ClickScheduler s;
    Counters c;
    ClickRenderer r(s, c);
    r.begin(48000);
    s.start(10000, 120, 4);  // first edge at 10 ms

    int16_t block[48];  // 1 ms blocks
    // blocks covering [0,10ms): silence
    for (int b = 0; b < 10; ++b) {
        r.render_block(block, 48, (uint64_t)b * 1000);
        for (int i = 0; i < 48; ++i) TEST_ASSERT_EQUAL_INT16(0, block[i]);
    }
    // block [10ms,11ms): click starts at offset 0
    r.render_block(block, 48, 10000);
    TEST_ASSERT_EQUAL_UINT32(1, c.clicks_rendered);
    bool nonzero = false;
    for (int i = 0; i < 48; ++i) nonzero |= (block[i] != 0);
    TEST_ASSERT_TRUE(nonzero);

    // voice continues across following blocks (25 ms long)
    r.render_block(block, 48, 11000);
    nonzero = false;
    for (int i = 0; i < 48; ++i) nonzero |= (block[i] != 0);
    TEST_ASSERT_TRUE(nonzero);
    TEST_ASSERT_EQUAL_UINT32(0, c.click_late_us_max);
}

static void test_render_mid_block_offset_and_late(void) {
    ClickScheduler s;
    Counters c;
    ClickRenderer r(s, c);
    r.begin(48000);
    s.start(10500, 120, 4);  // edge lands mid-block

    int16_t block[48];
    r.render_block(block, 48, 10000);  // window [10ms, 11ms)
    // first half silent, second half click
    for (int i = 0; i < 24; ++i) TEST_ASSERT_EQUAL_INT16(0, block[i]);
    bool nonzero = false;
    for (int i = 24; i < 48; ++i) nonzero |= (block[i] != 0);
    TEST_ASSERT_TRUE(nonzero);

    // scheduler started in the past (late edge): rendered immediately, counted
    ClickScheduler s2;
    Counters c2;
    ClickRenderer r2(s2, c2);
    r2.begin(48000);
    s2.start(5000, 120, 4);
    r2.render_block(block, 48, 9000);  // 4 ms late
    TEST_ASSERT_EQUAL_UINT32(1, c2.clicks_rendered);
    TEST_ASSERT_EQUAL_UINT32(4000, c2.click_late_us_max);
}

static void test_stopped_renders_silence(void) {
    ClickScheduler s;
    Counters c;
    ClickRenderer r(s, c);
    r.begin(48000);
    int16_t block[48];
    r.render_block(block, 48, 0);
    for (int i = 0; i < 48; ++i) TEST_ASSERT_EQUAL_INT16(0, block[i]);
    TEST_ASSERT_EQUAL_UINT32(0, c.clicks_rendered);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_beat_arithmetic_no_drift);
    RUN_TEST(test_accent_every_bar);
    RUN_TEST(test_next_edge_at_or_after);
    RUN_TEST(test_render_places_click_at_offset);
    RUN_TEST(test_render_mid_block_offset_and_late);
    RUN_TEST(test_stopped_renders_silence);
    return UNITY_END();
}
