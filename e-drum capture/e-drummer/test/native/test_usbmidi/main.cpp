// USB-MIDI event-packet parser: channel messages, system common, realtime,
// and multi-packet sysex assembly with the truncation cap.
#include <string.h>

#include <unity.h>

#include "edrum/classify.h"
#include "edrum/usbmidi.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

namespace {

struct Capture : UsbMidiParser::Handler {
    MidiMsg msgs[32];
    int n_msgs = 0;
    uint8_t syx[kSysexMax];
    int syx_len = -1;
    bool syx_trunc = false;
    int n_syx = 0;

    void on_midi(const MidiMsg& m) override {
        if (n_msgs < 32) msgs[n_msgs++] = m;
    }
    void on_sysex(const uint8_t* d, uint8_t len, bool trunc) override {
        memcpy(syx, d, len);
        syx_len = len;
        syx_trunc = trunc;
        ++n_syx;
    }
};

}  // namespace

static void test_note_on_off_cc(void) {
    Capture cap;
    UsbMidiParser p(cap);
    const uint8_t pkts[] = {
        0x09, 0x99, 36, 96,   // note_on ch9
        0x08, 0x89, 36, 0,    // note_off
        0x0B, 0xB9, 4, 88,    // CC4
        0x0E, 0xE9, 0, 64,    // pitchwheel center
        0x0C, 0xC9, 5, 0,     // program change
        0x0D, 0xD9, 70, 0,    // aftertouch
    };
    p.feed(pkts, sizeof(pkts));
    TEST_ASSERT_EQUAL_INT(6, cap.n_msgs);
    TEST_ASSERT_EQUAL_HEX8(0x99, cap.msgs[0].status);
    TEST_ASSERT_EQUAL_UINT8(36, cap.msgs[0].data1);
    TEST_ASSERT_EQUAL_UINT8(96, cap.msgs[0].data2);
    TEST_ASSERT_EQUAL_HEX8(0x89, cap.msgs[1].status);
    TEST_ASSERT_EQUAL_HEX8(0xB9, cap.msgs[2].status);
    TEST_ASSERT_EQUAL_HEX8(0xE9, cap.msgs[3].status);
    TEST_ASSERT_EQUAL_HEX8(0xC9, cap.msgs[4].status);
    TEST_ASSERT_EQUAL_HEX8(0xD9, cap.msgs[5].status);
    // classification (mirrors MessageStamper)
    TEST_ASSERT_TRUE(classify(cap.msgs[0]) == MsgClass::Event);
    TEST_ASSERT_TRUE(classify(cap.msgs[1]) == MsgClass::Ctrl);
    TEST_ASSERT_TRUE(classify(cap.msgs[2]) == MsgClass::Ctrl);
}

static void test_note_on_velocity_zero_is_ctrl(void) {
    MidiMsg m{0x99, 36, 0};
    TEST_ASSERT_TRUE(classify(m) == MsgClass::Ctrl);
}

static void test_realtime_classified_dropped(void) {
    Capture cap;
    UsbMidiParser p(cap);
    const uint8_t pkt[4] = {0x0F, 0xFE, 0, 0};  // active sensing (Roland ~3/s)
    p.feed_packet(pkt);
    TEST_ASSERT_EQUAL_INT(1, cap.n_msgs);
    TEST_ASSERT_TRUE(classify(cap.msgs[0]) == MsgClass::DropRealtime);
    const uint8_t clk[4] = {0x0F, 0xF8, 0, 0};
    p.feed_packet(clk);
    TEST_ASSERT_TRUE(classify(cap.msgs[1]) == MsgClass::DropRealtime);
}

static void test_sysex_multi_packet(void) {
    Capture cap;
    UsbMidiParser p(cap);
    // F0 41 10 | 00 11 22 | 33 F7 -> data = 41 10 00 11 22 33
    const uint8_t pkts[] = {
        0x04, 0xF0, 0x41, 0x10,  // start
        0x04, 0x00, 0x11, 0x22,  // continue
        0x06, 0x33, 0xF7, 0x00,  // end with 2 bytes
    };
    p.feed(pkts, sizeof(pkts));
    TEST_ASSERT_EQUAL_INT(1, cap.n_syx);
    TEST_ASSERT_EQUAL_INT(6, cap.syx_len);
    const uint8_t want[6] = {0x41, 0x10, 0x00, 0x11, 0x22, 0x33};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, cap.syx, 6);
    TEST_ASSERT_FALSE(cap.syx_trunc);
}

static void test_sysex_short_forms(void) {
    Capture cap;
    UsbMidiParser p(cap);
    // 3-byte sysex F0 7E F7 via CIN 0x7 (ends with 3 bytes)
    const uint8_t a[4] = {0x07, 0xF0, 0x7E, 0xF7};
    p.feed_packet(a);
    TEST_ASSERT_EQUAL_INT(1, cap.n_syx);
    TEST_ASSERT_EQUAL_INT(1, cap.syx_len);
    TEST_ASSERT_EQUAL_HEX8(0x7E, cap.syx[0]);
    // F0 F7 (empty) via CIN 0x6
    const uint8_t b[4] = {0x06, 0xF0, 0xF7, 0x00};
    p.feed_packet(b);
    TEST_ASSERT_EQUAL_INT(2, cap.n_syx);
    TEST_ASSERT_EQUAL_INT(0, cap.syx_len);
    // dangling end (CIN 0x5, lone F7) without start: ignored
    const uint8_t c[4] = {0x05, 0xF7, 0x00, 0x00};
    p.feed_packet(c);
    TEST_ASSERT_EQUAL_INT(2, cap.n_syx);
}

static void test_sysex_truncation_cap(void) {
    Capture cap;
    UsbMidiParser p(cap);
    const uint8_t start[4] = {0x04, 0xF0, 0x01, 0x02};
    p.feed_packet(start);
    for (int i = 0; i < 30; ++i) {  // 2 + 90 bytes > kSysexMax=48
        const uint8_t cont[4] = {0x04, 0x10, 0x11, 0x12};
        p.feed_packet(cont);
    }
    const uint8_t end[4] = {0x05, 0xF7, 0, 0};
    p.feed_packet(end);
    TEST_ASSERT_EQUAL_INT(1, cap.n_syx);
    TEST_ASSERT_EQUAL_INT((int)kSysexMax, cap.syx_len);
    TEST_ASSERT_TRUE(cap.syx_trunc);
}

static void test_system_common(void) {
    Capture cap;
    UsbMidiParser p(cap);
    const uint8_t pkts[] = {
        0x02, 0xF1, 0x35, 0x00,  // quarter frame
        0x03, 0xF2, 0x01, 0x02,  // song position
        0x05, 0xF6, 0x00, 0x00,  // tune request (single byte, not F7)
    };
    p.feed(pkts, sizeof(pkts));
    TEST_ASSERT_EQUAL_INT(3, cap.n_msgs);
    TEST_ASSERT_EQUAL_HEX8(0xF1, cap.msgs[0].status);
    TEST_ASSERT_EQUAL_HEX8(0xF2, cap.msgs[1].status);
    TEST_ASSERT_EQUAL_HEX8(0xF6, cap.msgs[2].status);
}

static void test_reserved_cins_ignored(void) {
    Capture cap;
    UsbMidiParser p(cap);
    const uint8_t pkts[] = {0x00, 1, 2, 3, 0x01, 4, 5, 6};
    p.feed(pkts, sizeof(pkts));
    TEST_ASSERT_EQUAL_INT(0, cap.n_msgs);
    TEST_ASSERT_EQUAL_INT(0, cap.n_syx);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_note_on_off_cc);
    RUN_TEST(test_note_on_velocity_zero_is_ctrl);
    RUN_TEST(test_realtime_classified_dropped);
    RUN_TEST(test_sysex_multi_packet);
    RUN_TEST(test_sysex_short_forms);
    RUN_TEST(test_sysex_truncation_cap);
    RUN_TEST(test_system_common);
    RUN_TEST(test_reserved_cins_ignored);
    return UNITY_END();
}
