// Wire framing for the device<->brain link (capture spec §13): COBS byte
// stuffing + CRC-16/CCITT-FALSE, transport-agnostic by construction — this
// module sees only byte buffers, never a transport. Both sides of the seam
// implement the identical discipline (brain: edrum/io/framing.py); the
// shared test vectors in test/native/test_frame and tests/test_devlink.py
// are the cross-language contract.
//
// Wire form of one frame:
//     0x00  COBS( payload || crc16_le(payload) )  0x00
// COBS output contains no zero bytes, so 0x00 is an unambiguous delimiter
// and a receiver can resynchronize mid-stream after arbitrary garbage
// (e.g. stray console text). The leading delimiter is emitted by writers as
// cheap resync insurance; FrameAccumulator tolerates any number of them.
//
// payload = channel u8 | type u8 | body...   (see sync_service.h)
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace edrum {

// Largest payload we ever carry: channel+type + FETCH response body (512)
// + a little header slack. CRC adds 2; COBS adds ceil(n/254) + 1.
constexpr size_t kFramePayloadMax = 520;
constexpr size_t kFrameEncodedMax = kFramePayloadMax + 2 /*crc*/ + 4 /*cobs*/ + 2 /*delims*/;

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflect, no xor-out.
// crc16("123456789") == 0x29B1. Chainable: pass a previous result as `seed`
// to continue over a split buffer — the sync protocol uses this to checksum
// large files in bounded chunks.
uint16_t crc16(const uint8_t* data, size_t len, uint16_t seed = 0xFFFF);

// COBS encode (streaming variant: always emits a final code byte, so input
// ending exactly on a 254-byte run gains a trailing 0x01 group — decoders
// accept both encodings). Returns encoded length, or 0 if out won't fit.
size_t cobs_encode(const uint8_t* in, size_t len, uint8_t* out, size_t cap);

// COBS decode. Returns decoded length, or SIZE_MAX on malformed input
// (embedded zero, truncated group, overflow). Empty input decodes to 0.
size_t cobs_decode(const uint8_t* in, size_t len, uint8_t* out, size_t cap);

// payload -> full wire frame (leading 0x00, COBS(payload||crc), 0x00).
// Returns frame length, or 0 if cap is too small / payload too large.
size_t frame_encode(const uint8_t* payload, size_t len, uint8_t* out, size_t cap);

// Byte-stream reassembly: feed bytes as they arrive; a completed,
// CRC-verified payload is delivered through push()'s out buffer. Garbage
// between delimiters (bad CRC, bad COBS, overflow) is dropped and counted —
// the stream resynchronizes on the next 0x00.
class FrameAccumulator {
public:
    // Feed one byte. Returns the payload length (> 0, CRC stripped) when a
    // valid frame just completed, else 0. `out` must hold kFramePayloadMax.
    size_t push(uint8_t b, uint8_t* out, size_t cap);

    void reset() {
        len_ = 0;
        overflow_ = false;
    }
    uint32_t bad_frames() const { return bad_; }

private:
    uint8_t buf_[kFrameEncodedMax];
    size_t len_ = 0;
    bool overflow_ = false;
    uint32_t bad_ = 0;
};

}  // namespace edrum
