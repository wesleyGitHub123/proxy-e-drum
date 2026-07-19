// Wire framing (frame.h): CRC-16/CCITT-FALSE vectors, COBS vectors and
// round-trips, and FrameAccumulator reassembly incl. resync after garbage.
// The COBS/CRC vectors here are the cross-language contract with the
// brain's mirror implementation (edrum/io/framing.py, tests/test_devlink.py)
// — a change that breaks them breaks the wire, not just a test.
#include <string.h>

#include <unity.h>

#include "edrum/frame.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

static void test_crc16_check_values(void) {
    // CRC-16/CCITT-FALSE canonical check value
    const uint8_t nine[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16(nine, sizeof(nine)));
    // empty input = the seed
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, crc16(nullptr, 0));
    // chaining: crc(a||b) == crc(b, seed=crc(a)) — the sync protocol's
    // bounded-chunk file checksum depends on this
    const uint16_t part = crc16(nine, 5);
    TEST_ASSERT_EQUAL_HEX16(crc16(nine, sizeof(nine)), crc16(nine + 5, 4, part));
}

static void test_cobs_vectors(void) {
    uint8_t out[600];

    // [] -> [01]
    TEST_ASSERT_EQUAL_UINT32(1, cobs_encode(nullptr, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x01, out[0]);

    // [00] -> [01 01]
    const uint8_t z[] = {0x00};
    TEST_ASSERT_EQUAL_UINT32(2, cobs_encode(z, 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x01, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[1]);

    // [11 22 00 33] -> [03 11 22 02 33]
    const uint8_t v[] = {0x11, 0x22, 0x00, 0x33};
    const uint8_t v_enc[] = {0x03, 0x11, 0x22, 0x02, 0x33};
    TEST_ASSERT_EQUAL_UINT32(5, cobs_encode(v, 4, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(v_enc, out, 5);

    // 254 non-zero bytes -> [FF ...254... 01] (streaming variant: trailing
    // empty group; decoders accept both encodings)
    uint8_t run[254];
    memset(run, 0x01, sizeof(run));
    TEST_ASSERT_EQUAL_UINT32(256, cobs_encode(run, 254, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[255]);

    uint8_t dec[600];
    TEST_ASSERT_EQUAL_UINT32(254, cobs_decode(out, 256, dec, sizeof(dec)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(run, dec, 254);
}

static void test_cobs_roundtrip_hard_cases(void) {
    uint8_t in[300], enc[400], dec[300];
    // zeros at every position of a small buffer
    for (size_t zpos = 0; zpos < 8; ++zpos) {
        for (size_t i = 0; i < 8; ++i) in[i] = (i == zpos) ? 0x00 : (uint8_t)(i + 1);
        const size_t e = cobs_encode(in, 8, enc, sizeof(enc));
        TEST_ASSERT_TRUE(e > 0);
        for (size_t i = 0; i < e; ++i) TEST_ASSERT_NOT_EQUAL(0x00, enc[i]);
        TEST_ASSERT_EQUAL_UINT32(8, cobs_decode(enc, e, dec, sizeof(dec)));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(in, dec, 8);
    }
    // lengths around the 254 group boundary, with and without zeros
    for (size_t len = 253; len <= 256; ++len) {
        for (size_t i = 0; i < len; ++i) in[i] = (uint8_t)((i % 251) + 1);
        const size_t e = cobs_encode(in, len, enc, sizeof(enc));
        TEST_ASSERT_TRUE(e > 0);
        TEST_ASSERT_EQUAL_UINT32(len, cobs_decode(enc, e, dec, sizeof(dec)));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(in, dec, len);

        in[len / 2] = 0x00;
        const size_t e2 = cobs_encode(in, len, enc, sizeof(enc));
        TEST_ASSERT_TRUE(e2 > 0);
        TEST_ASSERT_EQUAL_UINT32(len, cobs_decode(enc, e2, dec, sizeof(dec)));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(in, dec, len);
    }
}

static void test_cobs_decode_rejects_malformed(void) {
    uint8_t dec[64];
    // embedded zero
    const uint8_t z[] = {0x03, 0x11, 0x00};
    TEST_ASSERT_EQUAL_UINT32(SIZE_MAX, cobs_decode(z, 3, dec, sizeof(dec)));
    // truncated group (code promises 2 data bytes, only 1 present)
    const uint8_t t[] = {0x03, 0x11};
    TEST_ASSERT_EQUAL_UINT32(SIZE_MAX, cobs_decode(t, 2, dec, sizeof(dec)));
}

static size_t feed(FrameAccumulator& acc, const uint8_t* bytes, size_t n, uint8_t* out,
                   size_t cap, size_t* got_at = nullptr) {
    size_t plen = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t r = acc.push(bytes[i], out, cap);
        if (r > 0) {
            plen = r;
            if (got_at != nullptr) *got_at = i;
        }
    }
    return plen;
}

static void test_frame_roundtrip_through_accumulator(void) {
    const uint8_t payload[] = {0x00, 0x01, 0x01, 0x00};  // channel 0, HELLO, ver 1, no time
    uint8_t wire[kFrameEncodedMax];
    const size_t wn = frame_encode(payload, sizeof(payload), wire, sizeof(wire));
    TEST_ASSERT_TRUE(wn > 0);
    TEST_ASSERT_EQUAL_HEX8(0x00, wire[0]);       // leading delimiter
    TEST_ASSERT_EQUAL_HEX8(0x00, wire[wn - 1]);  // trailing delimiter

    FrameAccumulator acc;
    uint8_t got[kFramePayloadMax];
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload), feed(acc, wire, wn, got, sizeof(got)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, got, sizeof(payload));
    TEST_ASSERT_EQUAL_UINT32(0, acc.bad_frames());

    // two frames back-to-back arrive as two payloads
    uint8_t two[2 * kFrameEncodedMax];
    memcpy(two, wire, wn);
    memcpy(two + wn, wire, wn);
    int frames = 0;
    for (size_t i = 0; i < 2 * wn; ++i) {
        if (acc.push(two[i], got, sizeof(got)) > 0) ++frames;
    }
    TEST_ASSERT_EQUAL_INT(2, frames);
}

static void test_accumulator_resyncs_after_garbage(void) {
    const uint8_t payload[] = {0x00, 0x05};  // channel 0, BYE
    uint8_t wire[kFrameEncodedMax];
    const size_t wn = frame_encode(payload, sizeof(payload), wire, sizeof(wire));

    // stray console text before the frame: closed out by the frame's leading
    // delimiter, counted bad, then the real frame parses cleanly
    const char* text = "[click] 120 bpm /4\r\n";
    FrameAccumulator acc;
    uint8_t got[kFramePayloadMax];
    size_t plen = 0;
    for (const char* p = text; *p; ++p) {
        TEST_ASSERT_EQUAL_UINT32(0, acc.push((uint8_t)*p, got, sizeof(got)));
    }
    plen = feed(acc, wire, wn, got, sizeof(got));
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload), plen);
    TEST_ASSERT_EQUAL_UINT32(1, acc.bad_frames());
}

static void test_accumulator_rejects_corrupt_crc(void) {
    const uint8_t payload[] = {0x00, 0x02};  // channel 0, LIST
    uint8_t wire[kFrameEncodedMax];
    const size_t wn = frame_encode(payload, sizeof(payload), wire, sizeof(wire));
    wire[2] ^= 0x40;  // corrupt a data byte inside the COBS body

    FrameAccumulator acc;
    uint8_t got[kFramePayloadMax];
    TEST_ASSERT_EQUAL_UINT32(0, feed(acc, wire, wn, got, sizeof(got)));
    TEST_ASSERT_EQUAL_UINT32(1, acc.bad_frames());

    // stream recovers: a clean frame right after is delivered
    uint8_t wire2[kFrameEncodedMax];
    const size_t wn2 = frame_encode(payload, sizeof(payload), wire2, sizeof(wire2));
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload), feed(acc, wire2, wn2, got, sizeof(got)));
}

static void test_frame_encode_bounds(void) {
    uint8_t big[kFramePayloadMax + 1];
    memset(big, 0x42, sizeof(big));
    uint8_t wire[2 * kFrameEncodedMax];
    // payload at the max encodes; one past refuses
    TEST_ASSERT_TRUE(frame_encode(big, kFramePayloadMax, wire, sizeof(wire)) > 0);
    TEST_ASSERT_EQUAL_UINT32(0, frame_encode(big, sizeof(big), wire, sizeof(wire)));
    // zero-length payload refuses (a frame always carries channel+type)
    TEST_ASSERT_EQUAL_UINT32(0, frame_encode(big, 0, wire, sizeof(wire)));
    // insufficient output cap refuses rather than truncates
    TEST_ASSERT_EQUAL_UINT32(0, frame_encode(big, kFramePayloadMax, wire, 16));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_crc16_check_values);
    RUN_TEST(test_cobs_vectors);
    RUN_TEST(test_cobs_roundtrip_hard_cases);
    RUN_TEST(test_cobs_decode_rejects_malformed);
    RUN_TEST(test_frame_roundtrip_through_accumulator);
    RUN_TEST(test_accumulator_resyncs_after_garbage);
    RUN_TEST(test_accumulator_rejects_corrupt_crc);
    RUN_TEST(test_frame_encode_bounds);
    return UNITY_END();
}
