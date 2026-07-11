// USB-MIDI 1.0 event-packet parser (USB Device Class Definition for MIDI
// Devices §4): every USB-MIDI transfer is a sequence of 4-byte packets
// [cable<<4 | CIN, midi0, midi1, midi2]. This module turns transfer bytes
// into MidiMsg / sysex callbacks. Pure — natively testable with scripted
// packet sequences, including real TD-02 captures.
//
// Sysex is assembled here (data bytes only, F0/F7 stripped — mido's `data`
// convention, micro-decision 1) and capped at kSysexMax: overlong sysex is
// truncated and flagged, never dropped (the TD-02 emits no unsolicited bulk
// sysex during performance; the cap is a bounded-memory guarantee).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edrum/midimsg.h"
#include "edrum/records.h"

namespace edrum {

class UsbMidiParser {
public:
    struct Handler {
        virtual void on_midi(const MidiMsg& msg) = 0;
        virtual void on_sysex(const uint8_t* data, uint8_t len, bool truncated) = 0;
        virtual ~Handler() = default;
    };

    explicit UsbMidiParser(Handler& handler) : handler_(handler) {}

    // One 4-byte USB-MIDI event packet.
    void feed_packet(const uint8_t p[4]);

    // A whole transfer buffer; trailing bytes short of a packet are ignored
    // (the wire never produces them; defensive only).
    void feed(const uint8_t* buf, size_t len) {
        for (size_t i = 0; i + 4 <= len; i += 4) {
            feed_packet(buf + i);
        }
    }

private:
    void sysex_bytes(const uint8_t* b, uint8_t n);  // data bytes (F0/F7 stripped here)
    void sysex_end();

    Handler& handler_;
    uint8_t syx_buf_[kSysexMax];
    uint8_t syx_len_ = 0;
    bool syx_active_ = false;
    bool syx_truncated_ = false;
};

}  // namespace edrum
