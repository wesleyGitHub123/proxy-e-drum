// Experiment 1 gap classifier: the max is just the longest rest; the host-health
// signal is the lower tail (min + within-burst spacing). These tests pin the
// idle-vs-burst split, the thresholds, the histogram edges, and the reconnect
// reset — and reproduce the reported log shape (huge max, most gaps >10 ms) to
// prove idle rests never pollute the burst tail.
#include <unity.h>

#include "edrum/cb_gap.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

// Feed a first stamp then a run of cumulative gaps, so tests read as gap lists.
static void feed(CbGapStats& s, uint64_t t0, const uint32_t* gaps, int n) {
    s.record(t0);
    uint64_t t = t0;
    for (int i = 0; i < n; ++i) {
        t += gaps[i];
        s.record(t);
    }
}

static void test_first_sample_is_noop(void) {
    CbGapStats s;
    TEST_ASSERT_EQUAL_UINT32(0, s.record(500));  // no prior stamp -> gap 0
    TEST_ASSERT_EQUAL_UINT32(0, s.samples);      // and not counted
    TEST_ASSERT_EQUAL_UINT32(700, s.record(1200));  // now a real 700us gap
    TEST_ASSERT_EQUAL_UINT32(1, s.samples);
    TEST_ASSERT_EQUAL_UINT32(700, s.min_us);
    TEST_ASSERT_EQUAL_UINT32(700, s.max_us);
}

static void test_thresholds_are_strict(void) {
    // >2ms and >10ms are strict: the boundary value itself does not count.
    CbGapStats s;
    s.record(0);
    s.record(2000);            // gap == 2000: not > 2000
    TEST_ASSERT_EQUAL_UINT32(0, s.over_2ms);
    s.record(2000 + 2001);     // gap 2001: counts
    TEST_ASSERT_EQUAL_UINT32(1, s.over_2ms);

    CbGapStats t;
    t.record(0);
    t.record(10000);           // gap == 10000: not > 10000
    TEST_ASSERT_EQUAL_UINT32(0, t.over_10ms);
    t.record(10000 + 10001);   // gap 10001: counts
    TEST_ASSERT_EQUAL_UINT32(1, t.over_10ms);
}

static void test_burst_vs_idle_split(void) {
    // <=15ms is "data flowing"; wider is a rest. burst_max is the tail that
    // actually answers Experiment 1 — it must ignore the idle gaps entirely.
    CbGapStats s;
    const uint32_t gaps[] = {800, 15000 /*edge: still burst*/, 15001 /*rest*/,
                             3000, 500000 /*rest*/, 4000};
    feed(s, 0, gaps, 6);

    TEST_ASSERT_EQUAL_UINT32(6, s.samples);
    TEST_ASSERT_EQUAL_UINT32(500000, s.max_us);
    TEST_ASSERT_EQUAL_UINT32(4, s.burst_samples);   // 800,15000,3000,4000
    TEST_ASSERT_EQUAL_UINT32(15000, s.burst_max_us);  // NOT 500000
    TEST_ASSERT_EQUAL_UINT32(800, s.min_us);
}

static void test_histogram_edges(void) {
    // Bucketing is gap < edge; edges {1,2,5,10,50,200,1000}ms then overflow.
    CbGapStats s;
    const uint32_t gaps[] = {
        999,        // <1ms   -> b0
        1000,       // ==1ms  -> b1 (not < 1000)
        1999,       // <2ms   -> b1
        4999,       // <5ms   -> b2
        9000,       // <10ms  -> b3
        49999,      // <50ms  -> b4
        199999,     // <200ms -> b5
        999999,     // <1s    -> b6
        1000000,    // ==1s   -> b7 (overflow)
        5000000,    // >1s    -> b7
    };
    feed(s, 0, gaps, 10);

    TEST_ASSERT_EQUAL_UINT32(1, s.hist[0]);
    TEST_ASSERT_EQUAL_UINT32(2, s.hist[1]);  // 1000 and 1999
    TEST_ASSERT_EQUAL_UINT32(1, s.hist[2]);
    TEST_ASSERT_EQUAL_UINT32(1, s.hist[3]);
    TEST_ASSERT_EQUAL_UINT32(1, s.hist[4]);
    TEST_ASSERT_EQUAL_UINT32(1, s.hist[5]);
    TEST_ASSERT_EQUAL_UINT32(1, s.hist[6]);
    TEST_ASSERT_EQUAL_UINT32(2, s.hist[7]);  // 1000000 and 5000000
}

static void test_begin_stream_breaks_the_gap(void) {
    // A reconnect must not measure a gap across the disconnect, but must keep
    // the metrics accumulated so far.
    CbGapStats s;
    const uint32_t gaps[] = {700, 3000};
    feed(s, 0, gaps, 2);
    TEST_ASSERT_EQUAL_UINT32(2, s.samples);

    s.begin_stream();
    TEST_ASSERT_EQUAL_UINT32(0, s.record(9000000));  // huge idle -> not measured
    TEST_ASSERT_EQUAL_UINT32(2, s.samples);          // unchanged
    TEST_ASSERT_EQUAL_UINT32(3000, s.max_us);        // preserved

    s.record(9000000 + 600);  // fresh gap after the reconnect
    TEST_ASSERT_EQUAL_UINT32(3, s.samples);
    TEST_ASSERT_EQUAL_UINT32(600, s.min_us);
}

static void test_reported_run_shape(void) {
    // Reproduce the flagged log: a ~1.83 s max and most gaps > 10 ms, yet the
    // host path is healthy — the burst tail stays tight. This is the regression
    // guard for the "big max == bottleneck" misread.
    CbGapStats s;
    s.record(0);
    uint64_t t = 0;
    // 50 dense burst gaps near the frame floor (1-2 ms), then a long rest.
    for (int i = 0; i < 50; ++i) {
        t += 1200;
        s.record(t);
    }
    t += 1834657;  // the reported max: a rest, not a stall
    s.record(t);
    for (int i = 0; i < 50; ++i) {
        t += 1500;
        s.record(t);
    }

    TEST_ASSERT_EQUAL_UINT32(1834657, s.max_us);   // scary-looking...
    TEST_ASSERT_EQUAL_UINT32(1200, s.min_us);      // ...but the floor is tight
    TEST_ASSERT_EQUAL_UINT32(1500, s.burst_max_us);  // and the burst tail is 1.5 ms
    TEST_ASSERT_EQUAL_UINT32(100, s.burst_samples);  // both bursts, rest excluded
    TEST_ASSERT_EQUAL_UINT32(1, s.hist[7]);          // exactly one overflow: the rest
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_first_sample_is_noop);
    RUN_TEST(test_thresholds_are_strict);
    RUN_TEST(test_burst_vs_idle_split);
    RUN_TEST(test_histogram_edges);
    RUN_TEST(test_begin_stream_breaks_the_gap);
    RUN_TEST(test_reported_run_shape);
    return UNITY_END();
}
