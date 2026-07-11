// On-target smoke test (run with the board on the UART port):
//   pio test -e esp32-s3-devkitc-1
//
// Confirms the pure core behaves identically on xtensa (the serializer's
// byte output is architecture-independent) and that the real IClock is
// monotonic. The heavyweight validations (Experiments 1 and 3) are console-
// driven on the main firmware, not unit tests — see `stats` and `burst`.
#include <Arduino.h>
#include <string.h>
#include <unity.h>

#include "edrum/records.h"
#include "edrum/ring.h"
#include "edrum/serialize.h"
#include "edrum/platform/esp32_clock.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

static void test_clock_monotonic(void) {
    platform::Esp32Clock clk;
    uint64_t prev = clk.now_us();
    for (int i = 0; i < 1000; ++i) {
        const uint64_t now = clk.now_us();
        TEST_ASSERT_TRUE(now >= prev);
        prev = now;
    }
    const uint64_t a = clk.now_us();
    delay(10);
    const uint64_t b = clk.now_us();
    TEST_ASSERT_TRUE(b - a >= 9000 && b - a < 50000);  // ~10 ms elapsed
}

static void test_serializer_bytes_on_xtensa(void) {
    CaptureRecord r{};
    r.type = RecType::Event;
    r.t = 100;
    r.u.event = EventP{36, 96, 9};
    char buf[kMaxLine];
    const size_t n = record_line(r, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"event\",\"t\":100,\"note\":36,\"velocity\":96,\"channel\":9}\n", buf);
}

static void test_ring_on_target(void) {
    static SpscRing<CaptureRecord, 8> ring;
    CaptureRecord r{};
    r.type = RecType::Bookmark;
    for (uint32_t i = 0; i < 200; ++i) {
        r.t = i;
        TEST_ASSERT_TRUE(ring.push(r));
        CaptureRecord out{};
        TEST_ASSERT_TRUE(ring.pop(out));
        TEST_ASSERT_EQUAL_UINT32(i, out.t);
    }
}

void setup() {
    delay(2000);  // give the monitor time to attach
    UNITY_BEGIN();
    RUN_TEST(test_clock_monotonic);
    RUN_TEST(test_serializer_bytes_on_xtensa);
    RUN_TEST(test_ring_on_target);
    UNITY_END();
}

void loop() {}
