// SPSC ring buffer semantics: order, wrap, full/empty, high-water tracking.
#include <unity.h>

#include "edrum/records.h"
#include "edrum/ring.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

static CaptureRecord ev(uint32_t t) {
    CaptureRecord r{};
    r.type = RecType::Event;
    r.t = t;
    r.u.event = EventP{36, 100, 9};
    return r;
}

static void test_fifo_order(void) {
    SpscRing<CaptureRecord, 8> ring;
    TEST_ASSERT_TRUE(ring.empty());
    for (uint32_t i = 0; i < 5; ++i) TEST_ASSERT_TRUE(ring.push(ev(i)));
    TEST_ASSERT_EQUAL_UINT32(5, ring.size());
    CaptureRecord out{};
    for (uint32_t i = 0; i < 5; ++i) {
        TEST_ASSERT_TRUE(ring.pop(out));
        TEST_ASSERT_EQUAL_UINT32(i, out.t);
    }
    TEST_ASSERT_FALSE(ring.pop(out));
}

static void test_full_rejects_and_never_overwrites(void) {
    SpscRing<CaptureRecord, 4> ring;
    for (uint32_t i = 0; i < 4; ++i) TEST_ASSERT_TRUE(ring.push(ev(i)));
    TEST_ASSERT_FALSE(ring.push(ev(99)));  // full: reject, count upstream
    CaptureRecord out{};
    for (uint32_t i = 0; i < 4; ++i) {
        TEST_ASSERT_TRUE(ring.pop(out));
        TEST_ASSERT_EQUAL_UINT32(i, out.t);  // 99 never landed
    }
}

static void test_wraparound_many_times(void) {
    SpscRing<CaptureRecord, 4> ring;
    CaptureRecord out{};
    for (uint32_t i = 0; i < 1000; ++i) {
        TEST_ASSERT_TRUE(ring.push(ev(i)));
        TEST_ASSERT_TRUE(ring.pop(out));
        TEST_ASSERT_EQUAL_UINT32(i, out.t);
    }
    TEST_ASSERT_TRUE(ring.empty());
}

static void test_high_water(void) {
    SpscRing<CaptureRecord, 8> ring;
    CaptureRecord out{};
    ring.push(ev(0));
    ring.push(ev(1));
    ring.pop(out);
    ring.push(ev(2));
    ring.push(ev(3));
    ring.push(ev(4));  // depth peaks at 4
    TEST_ASSERT_EQUAL_UINT32(4, ring.high_water());
    while (ring.pop(out)) {
    }
    TEST_ASSERT_EQUAL_UINT32(4, ring.high_water());  // sticky
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_fifo_order);
    RUN_TEST(test_full_rejects_and_never_overwrites);
    RUN_TEST(test_wraparound_many_times);
    RUN_TEST(test_high_water);
    return UNITY_END();
}
