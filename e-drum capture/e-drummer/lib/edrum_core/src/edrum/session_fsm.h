// Session lifecycle state machine — the F3 subsystem. Pure logic over the
// IClock/IWallClock/IRandom ports and a RecordSink; fully deterministic
// under fake ports (the auto-pause/idle rules are exactly the kind of logic
// that is miserable to verify on-target and trivial to verify natively).
//
// Semantics (capture spec §5/§6, Jamcorder-derived):
//   * a session starts on the first EVENT (hit) or explicit declaration —
//     the box is always listening; no capture modes (brain spec invariant 1)
//   * `t` is session-relative integer ms on the one clock, monotonic,
//     NEVER rebased; auto-pause is an annotation-free stretch of timeline —
//     the clock counts through it (invariant 6)
//   * auto-pause after `pause_after_ms` of event silence: cacheable ctrl
//     (CC / pitchwheel / aftertouch / program_change) updates a state cache
//     instead of spamming lines; the delta is flushed on the resuming note
//   * on session START the full known controller state is flushed at t=0 so
//     every file is self-contained (hi-hat openness has a baseline)
//   * `idle_end_ms` of event/declaration silence ends the session
//     (the idle-timeout failsafe); open grid/enroll spans are closed first
//     so files end tidy
//   * grid/enroll declarations snapshot the RUNNING click (bpm/subdiv +
//     downbeat anchor) — the caller supplies the anchor in clock-µs and the
//     FSM converts it to session-t, keeping the one-clock rule structural
//
// Threading: every method must be called from the capture task (the single
// ring producer). Nothing here blocks.
#pragma once

#include <stdint.h>

#include "edrum/counters.h"
#include "edrum/hal/ports.h"
#include "edrum/midimsg.h"
#include "edrum/records.h"

namespace edrum {

class SessionController {
public:
    struct Cfg {
        uint32_t pause_after_ms = 3000;
        uint32_t idle_end_ms = 300000;
        int16_t tz_offset_min = 0;  // rendering hint for the wall clock
    };

    SessionController(const Cfg& cfg, RecordSink& sink, hal::IWallClock& wall,
                      hal::IRandom& random, Counters& counters)
        : cfg_(cfg), sink_(sink), wall_(wall), random_(random), counters_(counters) {}

    // --- MIDI path (from the USB adapter, post-classify) ------------------
    void on_event(uint64_t now_us, uint8_t note, uint8_t velocity, uint8_t channel);
    void on_ctrl(uint64_t now_us, const MidiMsg& msg);
    void on_sysex(uint64_t now_us, const uint8_t* data, uint8_t len, bool truncated);

    // --- periodic (capture task loop) --------------------------------------
    void tick(uint64_t now_us);

    // --- control surface (console / gestures / click) ----------------------
    void bookmark(uint64_t now_us);
    void grid_start(uint64_t now_us, uint16_t bpm, uint8_t subdiv, uint64_t downbeat_us);
    bool grid_end(uint64_t now_us);  // false if no span open
    void enroll_start(uint64_t now_us, const char* profile_ref, uint16_t bpm,
                      uint8_t subdiv, uint64_t downbeat_us);
    bool enroll_end(uint64_t now_us);
    void end_session(uint64_t now_us);

    // --- introspection ------------------------------------------------------
    bool in_session() const { return in_session_; }
    bool paused() const { return paused_; }
    bool grid_open() const { return grid_open_; }
    bool enroll_open() const { return enroll_open_; }
    uint32_t current_t(uint64_t now_us) const;

private:
    void ensure_session(uint64_t now_us);
    uint32_t stamp(uint64_t now_us);  // t mapping + monotonic clamp
    void push(const CaptureRecord& rec);
    void flush_cache(uint32_t t, bool full);  // full: all set; else dirty only
    void cache_update(const MidiMsg& msg, bool write_through, uint64_t now_us);

    Cfg cfg_;
    RecordSink& sink_;
    hal::IWallClock& wall_;
    hal::IRandom& random_;
    Counters& counters_;

    bool in_session_ = false;
    bool paused_ = false;
    bool grid_open_ = false;
    bool enroll_open_ = false;
    uint64_t session_start_us_ = 0;
    uint64_t last_event_us_ = 0;
    uint64_t last_activity_us_ = 0;
    uint32_t last_t_ = 0;

    // controller-state cache (16 channels)
    uint8_t cc_val_[16][128] = {};
    uint8_t cc_set_[16][16] = {};   // bitmaps
    uint8_t cc_dirty_[16][16] = {};
    uint8_t pc_val_[16] = {0};
    uint16_t pc_set_ = 0;
    uint16_t pc_dirty_ = 0;
    uint8_t at_val_[16] = {0};
    uint16_t at_set_ = 0;
    uint16_t at_dirty_ = 0;
    uint16_t pw_val_[16] = {0};  // raw 14-bit
    uint16_t pw_set_ = 0;
    uint16_t pw_dirty_ = 0;
};

}  // namespace edrum
