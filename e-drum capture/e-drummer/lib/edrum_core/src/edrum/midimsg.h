// Wire-level MIDI message, post-USB-parse (channel or system message).
// Sysex is handled out-of-band (variable length) — see usbmidi.h.
#pragma once

#include <stdint.h>

namespace edrum {

struct MidiMsg {
    uint8_t status;  // full status byte, e.g. 0x99 = note_on ch 9
    uint8_t data1;   // 0 when the message has no data bytes
    uint8_t data2;   // 0 when the message has one data byte

    uint8_t type_nibble() const { return status & 0xF0; }
    uint8_t channel() const { return status & 0x0F; }
    bool is_channel_message() const { return status >= 0x80 && status < 0xF0; }
    bool is_realtime() const { return status >= 0xF8; }
};

}  // namespace edrum
