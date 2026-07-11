#include "edrum/usbmidi.h"

namespace edrum {

void UsbMidiParser::sysex_bytes(const uint8_t* b, uint8_t n) {
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t byte = b[i];
        if (byte == 0xF0) {
            // (re)start — a new F0 aborts any half-finished message
            syx_active_ = true;
            syx_len_ = 0;
            syx_truncated_ = false;
            continue;
        }
        if (byte == 0xF7) {
            sysex_end();
            continue;
        }
        if (!syx_active_) continue;  // stray continuation without a start
        if (syx_len_ < kSysexMax) {
            syx_buf_[syx_len_++] = byte;
        } else {
            syx_truncated_ = true;
        }
    }
}

void UsbMidiParser::sysex_end() {
    if (!syx_active_) return;
    handler_.on_sysex(syx_buf_, syx_len_, syx_truncated_);
    syx_active_ = false;
    syx_len_ = 0;
    syx_truncated_ = false;
}

void UsbMidiParser::feed_packet(const uint8_t p[4]) {
    const uint8_t cin = p[0] & 0x0F;
    switch (cin) {
        case 0x0:  // misc / reserved
        case 0x1:  // cable events / reserved
            break;

        case 0x2:  // two-byte system common (F1, F3)
            handler_.on_midi(MidiMsg{p[1], p[2], 0});
            break;

        case 0x3:  // three-byte system common (F2)
            handler_.on_midi(MidiMsg{p[1], p[2], p[3]});
            break;

        case 0x4:  // sysex start or continue (3 bytes)
            sysex_bytes(p + 1, 3);
            break;

        case 0x5:  // single-byte system common, or sysex end with 1 byte
            if (p[1] == 0xF7) {
                sysex_bytes(p + 1, 1);
            } else {
                handler_.on_midi(MidiMsg{p[1], 0, 0});
            }
            break;

        case 0x6:  // sysex ends with 2 bytes
            sysex_bytes(p + 1, 2);
            break;

        case 0x7:  // sysex ends with 3 bytes
            sysex_bytes(p + 1, 3);
            break;

        case 0x8:  // note_off
        case 0x9:  // note_on
        case 0xA:  // polytouch
        case 0xB:  // control_change
        case 0xE:  // pitchwheel
            handler_.on_midi(MidiMsg{p[1], p[2], p[3]});
            break;

        case 0xC:  // program_change (2 bytes)
        case 0xD:  // channel aftertouch (2 bytes)
            handler_.on_midi(MidiMsg{p[1], p[2], 0});
            break;

        case 0xF:  // single byte (system realtime)
            handler_.on_midi(MidiMsg{p[1], 0, 0});
            break;
    }
}

}  // namespace edrum
