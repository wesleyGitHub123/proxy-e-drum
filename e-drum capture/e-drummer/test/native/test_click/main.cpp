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

// Decision 4: plan (locked, integer decisions) and mix (unlocked, sample
// work) split — the two-phase path must produce byte-identical audio to the
// single-call convenience path.
static void test_plan_mix_split_equals_render(void) {
    ClickScheduler s1, s2;
    Counters c1, c2;
    ClickRenderer whole(s1, c1), split(s2, c2);
    whole.begin(48000);
    split.begin(48000);
    s1.start(10500, 120, 4);
    s2.start(10500, 120, 4);

    int16_t a[48], b[48];
    for (int blk = 0; blk < 40; ++blk) {  // spans silence, trigger, tail
        const uint64_t t0 = (uint64_t)blk * 1000;
        whole.render_block(a, 48, t0);
        ClickRenderer::BlockPlan plan;
        split.plan_block(plan, 48, t0);
        split.mix_block(b, 48, plan);
        TEST_ASSERT_EQUAL_INT16_ARRAY(a, b, 48);
    }
    TEST_ASSERT_EQUAL_UINT32(c1.clicks_rendered, c2.clicks_rendered);
}

// Decision 2: gain scales the pre-rendered voices at begin() — output
// samples at 50% are exactly half of the 100% samples, and 0% is silence
// while still counting edges (the schedule is authoritative, not the sound).
static void test_gain_scales_output(void) {
    ClickScheduler s1, s2;
    Counters c1, c2;
    ClickRenderer full(s1, c1), half(s2, c2);
    full.begin(48000, 100);
    half.begin(48000, 50);
    s1.start(0, 120, 4);
    s2.start(0, 120, 4);

    int16_t a[48], b[48];
    full.render_block(a, 48, 0);
    half.render_block(b, 48, 0);
    bool nonzero = false;
    for (int i = 0; i < 48; ++i) {
        TEST_ASSERT_EQUAL_INT16((int16_t)((int32_t)a[i] * 50 / 100), b[i]);
        nonzero |= (a[i] != 0);
    }
    TEST_ASSERT_TRUE(nonzero);

    ClickScheduler s3;
    Counters c3;
    ClickRenderer mute(s3, c3);
    mute.begin(48000, 0);
    s3.start(0, 120, 4);
    mute.render_block(a, 48, 0);
    for (int i = 0; i < 48; ++i) TEST_ASSERT_EQUAL_INT16(0, a[i]);
    TEST_ASSERT_EQUAL_UINT32(1, c3.clicks_rendered);
}

// quiet() while the schedule is stopped: a restart never resumes the stale
// voice mid-decay (the click task calls this in its idle branch).
static void test_quiet_drops_stale_voice(void) {
    ClickScheduler s;
    Counters c;
    ClickRenderer r(s, c);
    r.begin(48000);
    s.start(0, 120, 4);

    int16_t block[48];
    r.render_block(block, 48, 0);  // voice active (25 ms > 1 ms block)
    s.stop();
    r.quiet();

    s.start(100000, 120, 4);  // restart; first edge at 100 ms
    r.render_block(block, 48, 50000);  // pre-edge block must be silent
    for (int i = 0; i < 48; ++i) TEST_ASSERT_EQUAL_INT16(0, block[i]);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_beat_arithmetic_no_drift);
    RUN_TEST(test_accent_every_bar);
    RUN_TEST(test_next_edge_at_or_after);
    RUN_TEST(test_render_places_click_at_offset);
    RUN_TEST(test_render_mid_block_offset_and_late);
    RUN_TEST(test_stopped_renders_silence);
    RUN_TEST(test_plan_mix_split_equals_render);
    RUN_TEST(test_gain_scales_output);
    RUN_TEST(test_quiet_drops_stale_voice);
    return UNITY_END();
}
