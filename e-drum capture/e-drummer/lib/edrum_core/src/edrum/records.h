// The in-RAM capture record — the single item type carried by the ring buffer
// and consumed by Storage. This is the firmware analog of the brain's typed
// records (brain spec §3; edrum/engine/records.py): every subsystem meets at
// this type, and Storage serializes it to the canonical on-disk line.
//
// Fixed-size POD (union) so the SPSC ring never allocates. `SessionStart` is a
// marker, not a log line: it instructs Storage to open a new session file and
// write the meta line (line 1). Everything else maps 1:1 to a log line.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edrum/midimsg.h"

namespace edrum {

// Serialization major version (mirror of edrum.engine.records.SCHEMA_VERSION).
constexpr int kSchemaVersion = 1;

constexpr size_t kSysexMax = 48;       // sysex data bytes kept (excl. F0/F7)
constexpr size_t kProfileRefMax = 32;  // enroll profile_ref
constexpr size_t kSessionIdLen = 32;   // uuid4 hex
constexpr size_t kIsoMax = 25;         // "2026-07-06T12:00:00+08:00"
constexpr size_t kNameMax = 32;        // user_id / kit_profile_id

enum class RecType : uint8_t {
    SessionStart,  // marker: open file, write meta (not itself a line)
    Event,         // note_on velocity>0 — the (t, note, velocity) atom
    CtrlMidi,      // any other channel/system-common message, raw
    CtrlSysex,     // sysex payload (data bytes, F0/F7 stripped)
    GridStart,
    GridEnd,
    Bookmark,
    EnrollStart,
    EnrollEnd,
    SessionEnd,
};

struct SessionStartP {
    char session_id[kSessionIdLen + 1];
    char start_iso[kIsoMax + 1];
};

struct EventP {
    uint8_t note;
    uint8_t velocity;
    uint8_t channel;  // 0-15
};

struct CtrlMidiP {
    MidiMsg msg;
};

struct CtrlSysexP {
    uint8_t len;
    uint8_t truncated;  // 1 = wire sysex exceeded kSysexMax (counted upstream)
    uint8_t data[kSysexMax];
};

struct GridP {
    uint16_t bpm;
    uint8_t subdiv;
    uint32_t downbeat_t;  // session-relative ms, same clock as t (spec §3 Rule 1)
};

struct EnrollP {
    char profile_ref[kProfileRefMax + 1];
    uint16_t bpm;
    uint8_t subdiv;
    uint32_t downbeat_t;
};

struct CaptureRecord {
    RecType type;
    uint32_t t;  // session-relative integer ms, monotonic, never rebased
    union {
        SessionStartP session_start;
        EventP event;
        CtrlMidiP ctrl;
        CtrlSysexP sysex;
        GridP grid;
        EnrollP enroll;
    } u;
};

// The producer→consumer seam. The capture side pushes; it never knows what
// consumes (Storage, a test sink, a counting no-op). Push returns false when
// the sink is full — the caller counts drops; nothing ever blocks.
struct RecordSink {
    virtual bool push(const CaptureRecord& rec) = 0;
    virtual ~RecordSink() = default;
};

}  // namespace edrum
