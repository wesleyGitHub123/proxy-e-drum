// Gesture grammar: chords must be tight, sequences must be quick, and
// ordinary playing must never trigger anything.
#include <unity.h>

#include "edrum/gesture.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

static GestureDetector::Cfg cfg() {
    GestureDetector::Cfg c;
    c.enabled = true;
    c.note_a = 36;
    c.note_b = 49;
    c.min_velocity = 30;
    c.chord_window_ms = 120;
    c.seq_gap_ms = 700;
    return c;
}

static void chord(GestureDetector& g, uint32_t t) {
    g.on_event(t, 36, 100);
    g.on_event(t + 20, 49, 100);
}

static void test_two_chords_bookmark(void) {
    GestureDetector g(cfg());
    chord(g, 1000);
    chord(g, 1500);
    TEST_ASSERT_TRUE(g.poll(1600) == GestureDetector::Action::None);  // may grow
    TEST_ASSERT_TRUE(g.poll(2300) == GestureDetector::Action::Bookmark);
    TEST_ASSERT_TRUE(g.poll(2400) == GestureDetector::Action::None);  // consumed
}

static void test_three_chords_grid_toggle(void) {
    GestureDetector g(cfg());
    chord(g, 1000);
    chord(g, 1500);
    chord(g, 2000);
    TEST_ASSERT_TRUE(g.poll(2800) == GestureDetector::Action::GridToggle);
}

static void test_four_chords_enroll_toggle(void) {
    GestureDetector g(cfg());
    chord(g, 1000);
    chord(g, 1500);
    chord(g, 2000);
    chord(g, 2500);
    TEST_ASSERT_TRUE(g.poll(3300) == GestureDetector::Action::EnrollToggle);
}

static void test_single_chord_is_just_playing(void) {
    GestureDetector g(cfg());
    chord(g, 1000);
    TEST_ASSERT_TRUE(g.poll(1800) == GestureDetector::Action::None);
}

static void test_slow_pair_no_chord(void) {
    GestureDetector g(cfg());
    g.on_event(1000, 36, 100);
    g.on_event(1300, 49, 100);  // 300 ms apart: not a chord
    g.on_event(1500, 36, 100);
    g.on_event(1520, 49, 100);  // chord (count 1)
    TEST_ASSERT_TRUE(g.poll(2300) == GestureDetector::Action::None);
}

static void test_stale_sequence_restarts(void) {
    GestureDetector g(cfg());
    chord(g, 1000);
    chord(g, 3000);  // 2 s after: stale — restarts, count 1
    TEST_ASSERT_TRUE(g.poll(3800) == GestureDetector::Action::None);
}

static void test_soft_hits_and_other_pads_ignored(void) {
    GestureDetector g(cfg());
    g.on_event(1000, 36, 10);   // too soft
    g.on_event(1010, 49, 100);
    TEST_ASSERT_TRUE(g.poll(1800) == GestureDetector::Action::None);

    // snare hits between chord halves don't break the chord
    g.on_event(2000, 36, 100);
    g.on_event(2010, 38, 90);
    g.on_event(2030, 49, 100);
    chord(g, 2500);
    TEST_ASSERT_TRUE(g.poll(3300) == GestureDetector::Action::Bookmark);
}

static void test_disabled(void) {
    auto c = cfg();
    c.enabled = false;
    GestureDetector g(c);
    chord(g, 1000);
    chord(g, 1500);
    TEST_ASSERT_TRUE(g.poll(2300) == GestureDetector::Action::None);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_two_chords_bookmark);
    RUN_TEST(test_three_chords_grid_toggle);
    RUN_TEST(test_four_chords_enroll_toggle);
    RUN_TEST(test_single_chord_is_just_playing);
    RUN_TEST(test_slow_pair_no_chord);
    RUN_TEST(test_stale_sequence_restarts);
    RUN_TEST(test_soft_hits_and_other_pads_ignored);
    RUN_TEST(test_disabled);
    return UNITY_END();
}
