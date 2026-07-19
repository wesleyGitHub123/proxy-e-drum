#include "edrum/frame.h"

#include <string.h>

namespace edrum {

uint16_t crc16(const uint8_t* data, size_t len, uint16_t seed) {
    uint16_t crc = seed;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t cobs_encode(const uint8_t* in, size_t len, uint8_t* out, size_t cap) {
    if (cap == 0) return 0;
    size_t out_i = 0;
    size_t code_i = out_i++;
    uint8_t code = 1;
    for (size_t i = 0; i < len; ++i) {
        if (in[i] == 0) {
            out[code_i] = code;
            if (out_i >= cap) return 0;
            code_i = out_i++;
            code = 1;
        } else {
            if (out_i >= cap) return 0;
            out[out_i++] = in[i];
            if (++code == 0xFF) {
                out[code_i] = code;
                if (out_i >= cap) return 0;
                code_i = out_i++;
                code = 1;
            }
        }
    }
    out[code_i] = code;
    return out_i;
}

size_t cobs_decode(const uint8_t* in, size_t len, uint8_t* out, size_t cap) {
    size_t out_i = 0;
    size_t i = 0;
    while (i < len) {
        const uint8_t code = in[i++];
        if (code == 0) return SIZE_MAX;  // delimiter must not appear inside
        for (uint8_t j = 1; j < code; ++j) {
            if (i >= len) return SIZE_MAX;  // truncated group
            if (in[i] == 0) return SIZE_MAX;
            if (out_i >= cap) return SIZE_MAX;
            out[out_i++] = in[i++];
        }
        if (code != 0xFF && i < len) {
            if (out_i >= cap) return SIZE_MAX;
            out[out_i++] = 0;
        }
    }
    return out_i;
}

size_t frame_encode(const uint8_t* payload, size_t len, uint8_t* out, size_t cap) {
    if (len == 0 || len > kFramePayloadMax || cap < 3) return 0;

    uint8_t staged[kFramePayloadMax + 2];
    memcpy(staged, payload, len);
    const uint16_t crc = crc16(payload, len);
    staged[len] = (uint8_t)(crc & 0xFF);
    staged[len + 1] = (uint8_t)(crc >> 8);

    out[0] = 0x00;  // leading delimiter: cheap resync insurance
    const size_t enc = cobs_encode(staged, len + 2, out + 1, cap - 2);
    if (enc == 0) return 0;
    out[1 + enc] = 0x00;
    return enc + 2;
}

size_t FrameAccumulator::push(uint8_t b, uint8_t* out, size_t cap) {
    if (b != 0x00) {
        if (len_ < sizeof(buf_)) {
            buf_[len_++] = b;
        } else {
            overflow_ = true;
        }
        return 0;
    }

    // delimiter: close out whatever accumulated
    const size_t len = len_;
    const bool overflow = overflow_;
    len_ = 0;
    overflow_ = false;
    if (len == 0) return 0;  // empty segment (leading/duplicate delimiter)
    if (overflow) {
        ++bad_;
        return 0;
    }

    uint8_t decoded[kFramePayloadMax + 2];
    const size_t n = cobs_decode(buf_, len, decoded, sizeof(decoded));
    if (n == SIZE_MAX || n < 3) {  // need >= 1 payload byte + 2 crc bytes
        ++bad_;
        return 0;
    }
    const size_t payload_len = n - 2;
    const uint16_t got = (uint16_t)(decoded[payload_len] | ((uint16_t)decoded[payload_len + 1] << 8));
    if (crc16(decoded, payload_len) != got) {
        ++bad_;
        return 0;
    }
    if (payload_len > cap) {
        ++bad_;
        return 0;
    }
    memcpy(out, decoded, payload_len);
    return payload_len;
}

}  // namespace edrum
