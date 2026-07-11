// Message classification — the firmware mirror of the brain's
// MessageStamper (edrum/io/sources.py):
//   * system-realtime dropped at capture (micro-decision 2)
//   * note_on velocity>0  -> event (the performance-track atom)
//   * everything else     -> ctrl, raw (micro-decisions 1 & 3 — note_off and
//     note_on-velocity-0 are preserved-but-invisible ctrl records)
//
// The timestamp is NOT taken here: it is taken by the caller at the arrival
// edge, BEFORE parsing or classification (capture spec §3 Rule 2).
#pragma once

#include "edrum/midimsg.h"

namespace edrum {

enum class MsgClass : uint8_t {
    DropRealtime,  // F8..FF — transport keepalive, not performance information
    Event,         // note_on velocity>0
    Ctrl,          // preserved raw as a ctrl record
};

inline MsgClass classify(const MidiMsg& m) {
    if (m.is_realtime()) return MsgClass::DropRealtime;
    if (m.type_nibble() == 0x90 && m.data2 > 0) return MsgClass::Event;
    return MsgClass::Ctrl;
}

}  // namespace edrum
