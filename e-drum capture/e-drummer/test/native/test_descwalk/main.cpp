// Config-descriptor walk against a synthetic blob shaped like a class-
// compliant USB-MIDI device (audio control iface 0, MIDI streaming iface 1
// with class-specific descriptors and bulk IN/OUT endpoints — the TD-02
// pattern observed by the enumeration probe).
#include <unity.h>

#include "edrum/descwalk.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

// clang-format off
static const uint8_t kMidiDeviceCfg[] = {
    // config descriptor header (9 bytes)
    9, 0x02, 0x65, 0x00, 2, 1, 0, 0x80, 50,
    // interface 0: audio control (0x01/0x01)
    9, 0x04, 0, 0, 0, 0x01, 0x01, 0x00, 0,
    //   class-specific AC header
    9, 0x24, 0x01, 0x00, 0x01, 9, 0, 1, 1,
    // interface 1: MIDI streaming (0x01/0x03), 2 endpoints
    9, 0x04, 1, 0, 2, 0x01, 0x03, 0x00, 0,
    //   class-specific MS header
    7, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00,
    //   MIDI IN jack (embedded)
    6, 0x24, 0x02, 0x01, 0x01, 0x00,
    //   MIDI OUT jack (embedded)
    9, 0x24, 0x03, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00,
    // endpoint OUT 0x01, bulk, MPS 64
    7, 0x05, 0x01, 0x02, 64, 0, 0,
    //   class-specific EP
    5, 0x25, 0x01, 0x01, 0x01,
    // endpoint IN 0x81, bulk, MPS 64
    7, 0x05, 0x81, 0x02, 64, 0, 0,
    //   class-specific EP
    5, 0x25, 0x01, 0x01, 0x02,
};
// clang-format on

static void test_finds_midi_streaming(void) {
    const MidiStreamingInfo info =
        find_midi_streaming(kMidiDeviceCfg, (uint16_t)sizeof(kMidiDeviceCfg));
    TEST_ASSERT_TRUE(info.found);
    TEST_ASSERT_EQUAL_UINT8(1, info.interface_number);
    TEST_ASSERT_EQUAL_UINT8(0, info.alt_setting);
    TEST_ASSERT_TRUE(info.has_in_ep);
    TEST_ASSERT_EQUAL_HEX8(0x81, info.in_ep_addr);
    TEST_ASSERT_EQUAL_UINT16(64, info.in_ep_mps);
    TEST_ASSERT_EQUAL_UINT8(0x02, info.in_ep_attr);  // bulk
    TEST_ASSERT_TRUE(info.has_out_ep);
    TEST_ASSERT_EQUAL_HEX8(0x01, info.out_ep_addr);
}

static void test_no_midi_interface(void) {
    // HID-only device
    static const uint8_t hid[] = {
        9, 0x02, 0x22, 0x00, 1, 1, 0, 0x80, 50,
        9, 0x04, 0, 0, 1, 0x03, 0x01, 0x01, 0,
        7, 0x05, 0x81, 0x03, 8, 0, 0,
    };
    const MidiStreamingInfo info = find_midi_streaming(hid, (uint16_t)sizeof(hid));
    TEST_ASSERT_FALSE(info.found);
}

static void test_defensive_on_garbage(void) {
    static const uint8_t garbage[] = {9, 0x02, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0};
    const MidiStreamingInfo a = find_midi_streaming(garbage, (uint16_t)sizeof(garbage));
    TEST_ASSERT_FALSE(a.found);
    const MidiStreamingInfo b = find_midi_streaming(nullptr, 100);
    TEST_ASSERT_FALSE(b.found);
    // zero-length descriptor must not loop forever
    static const uint8_t zero[] = {9, 0x02, 0x20, 0x00, 1, 1, 0, 0x80, 50, 0, 0, 0};
    const MidiStreamingInfo c = find_midi_streaming(zero, (uint16_t)sizeof(zero));
    TEST_ASSERT_FALSE(c.found);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_finds_midi_streaming);
    RUN_TEST(test_no_midi_interface);
    RUN_TEST(test_defensive_on_garbage);
    return UNITY_END();
}
