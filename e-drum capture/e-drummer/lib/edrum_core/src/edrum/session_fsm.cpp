#include "edrum/session_fsm.h"

#include <string.h>

#include "edrum/timefmt.h"

namespace edrum {

namespace {

inline bool bit_get(const uint8_t* map, uint8_t i) {
    return (map[i >> 3] >> (i & 7)) & 1;
}
inline void bit_set(uint8_t* map, uint8_t i) {
    map[i >> 3] = (uint8_t)(map[i >> 3] | (1u << (i & 7)));
}
inline void bit_clr(uint8_t* map, uint8_t i) {
    map[i >> 3] = (uint8_t)(map[i >> 3] & ~(1u << (i & 7)));
}

// Cacheable = channel STATE, not occurrences: CC, program, channel pressure,
// pitch wheel. Everything else passes through raw.
inline bool cacheable(const MidiMsg& m) {
    const uint8_t hi = m.type_nibble();
    return m.is_channel_message() &&
           (hi == 0xB0 || hi == 0xC0 || hi == 0xD0 || hi == 0xE0);
}

}  // namespace

uint32_t SessionController::current_t(uint64_t now_us) const {
    if (!in_session_) return 0;
    const uint64_t t = (now_us - session_start_us_) / 1000;
    return t < last_t_ ? last_t_ : (uint32_t)t;
}

uint32_t SessionController::stamp(uint64_t now_us) {
    uint64_t t = (now_us - session_start_us_) / 1000;
    if (t < last_t_) t = last_t_;  // the clock is monotonic; belt-and-braces
    last_t_ = (uint32_t)t;
    return last_t_;
}

void SessionController::push(const CaptureRecord& rec) {
    if (!sink_.push(rec)) {
        counters_.ring_drops++;  // must stay 0 — Experiment 3's hard criterion
    }
}

void SessionController::ensure_session(uint64_t now_us) {
    if (in_session_) return;

    session_start_us_ = now_us;
    last_t_ = 0;
    last_event_us_ = now_us;
    last_activity_us_ = now_us;
    paused_ = false;
    in_session_ = true;

    CaptureRecord r{};
    r.type = RecType::SessionStart;
    r.t = 0;

    uint8_t raw[16];
    random_.fill(raw, sizeof(raw));
    uuid4_hex(raw, r.u.session_start.session_id);

    DateTime dt;
    if (wall_.now(dt) &&
        format_iso(dt, r.u.session_start.start_iso, sizeof(r.u.session_start.start_iso)) > 0) {
        // start_iso rendered from the wall clock
    } else {
        // The platform wall clock guarantees ordered timestamps (ADR-5);
        // this fallback only fires with a broken adapter.
        strcpy(r.u.session_start.start_iso, "1970-01-01T00:00:00+00:00");
    }

    push(r);
    counters_.sessions_started++;

    // Self-contained file: flush the full known controller state at t=0
    // (the hi-hat CC baseline the brain's openness derivation needs).
    flush_cache(0, /*full=*/true);
}

void SessionController::flush_cache(uint32_t t, bool full) {
    bool flushed = false;
    for (uint8_t ch = 0; ch < 16; ++ch) {
        const uint16_t chbit = (uint16_t)(1u << ch);

        if ((full ? pc_set_ : pc_dirty_) & chbit) {
            CaptureRecord r{};
            r.type = RecType::CtrlMidi;
            r.t = t;
            r.u.ctrl.msg = MidiMsg{(uint8_t)(0xC0 | ch), pc_val_[ch], 0};
            push(r);
            flushed = true;
        }
        for (uint16_t cc = 0; cc < 128; ++cc) {
            const bool want = full ? bit_get(cc_set_[ch], (uint8_t)cc)
                                   : bit_get(cc_dirty_[ch], (uint8_t)cc);
            if (!want) continue;
            CaptureRecord r{};
            r.type = RecType::CtrlMidi;
            r.t = t;
            r.u.ctrl.msg = MidiMsg{(uint8_t)(0xB0 | ch), (uint8_t)cc, cc_val_[ch][cc]};
            push(r);
            flushed = true;
        }
        if ((full ? at_set_ : at_dirty_) & chbit) {
            CaptureRecord r{};
            r.type = RecType::CtrlMidi;
            r.t = t;
            r.u.ctrl.msg = MidiMsg{(uint8_t)(0xD0 | ch), at_val_[ch], 0};
            push(r);
            flushed = true;
        }
        if ((full ? pw_set_ : pw_dirty_) & chbit) {
            CaptureRecord r{};
            r.type = RecType::CtrlMidi;
            r.t = t;
            r.u.ctrl.msg = MidiMsg{(uint8_t)(0xE0 | ch), (uint8_t)(pw_val_[ch] & 0x7F),
                                   (uint8_t)(pw_val_[ch] >> 7)};
            push(r);
            flushed = true;
        }
    }
    memset(cc_dirty_, 0, sizeof(cc_dirty_));
    pc_dirty_ = at_dirty_ = pw_dirty_ = 0;
    if (flushed) counters_.ctrl_cache_flushes++;
}

void SessionController::cache_update(const MidiMsg& msg, bool write_through,
                                     uint64_t now_us) {
    const uint8_t ch = msg.channel();
    const uint16_t chbit = (uint16_t)(1u << ch);
    switch (msg.type_nibble()) {
        case 0xB0:
            cc_val_[ch][msg.data1 & 0x7F] = msg.data2;
            bit_set(cc_set_[ch], msg.data1 & 0x7F);
            if (write_through) {
                bit_clr(cc_dirty_[ch], msg.data1 & 0x7F);
            } else {
                bit_set(cc_dirty_[ch], msg.data1 & 0x7F);
            }
            break;
        case 0xC0:
            pc_val_[ch] = msg.data1;
            pc_set_ |= chbit;
            if (write_through) pc_dirty_ &= (uint16_t)~chbit; else pc_dirty_ |= chbit;
            break;
        case 0xD0:
            at_val_[ch] = msg.data1;
            at_set_ |= chbit;
            if (write_through) at_dirty_ &= (uint16_t)~chbit; else at_dirty_ |= chbit;
            break;
        case 0xE0:
            pw_val_[ch] = (uint16_t)(((uint16_t)msg.data2 << 7) | msg.data1);
            pw_set_ |= chbit;
            if (write_through) pw_dirty_ &= (uint16_t)~chbit; else pw_dirty_ |= chbit;
            break;
        default:
            return;
    }
    if (write_through) {
        CaptureRecord r{};
        r.type = RecType::CtrlMidi;
        r.t = stamp(now_us);
        r.u.ctrl.msg = msg;
        push(r);
        counters_.ctrls++;
    }
}

void SessionController::on_event(uint64_t now_us, uint8_t note, uint8_t velocity,
                                 uint8_t channel) {
    const bool starting = !in_session_;
    ensure_session(now_us);

    if (!starting && paused_) {
        // resume: write the controller-state delta accumulated in the pause,
        // stamped at the resuming hit (Jamcorder's "write it on first note")
        flush_cache(stamp(now_us), /*full=*/false);
        paused_ = false;
    }

    CaptureRecord r{};
    r.type = RecType::Event;
    r.t = stamp(now_us);
    r.u.event = EventP{note, velocity, (uint8_t)(channel & 0x0F)};
    push(r);
    counters_.events++;

    last_event_us_ = now_us;
    last_activity_us_ = now_us;
}

void SessionController::on_ctrl(uint64_t now_us, const MidiMsg& msg) {
    if (cacheable(msg)) {
        const bool write_through = in_session_ && !paused_;
        cache_update(msg, write_through, now_us);
        return;
    }
    // Non-cacheable occurrences (note_off, note_on v0, polytouch, system
    // common): raw pass-through when a session exists; orphaned otherwise.
    if (!in_session_) {
        counters_.ctrl_nosession_dropped++;
        return;
    }
    CaptureRecord r{};
    r.type = RecType::CtrlMidi;
    r.t = stamp(now_us);
    r.u.ctrl.msg = msg;
    push(r);
    counters_.ctrls++;
}

void SessionController::on_sysex(uint64_t now_us, const uint8_t* data, uint8_t len,
                                 bool truncated) {
    if (!in_session_) {
        counters_.ctrl_nosession_dropped++;
        return;
    }
    CaptureRecord r{};
    r.type = RecType::CtrlSysex;
    r.t = stamp(now_us);
    r.u.sysex.len = len > kSysexMax ? (uint8_t)kSysexMax : len;
    r.u.sysex.truncated = truncated ? 1 : 0;
    memcpy(r.u.sysex.data, data, r.u.sysex.len);
    push(r);
    counters_.ctrls++;
}

void SessionController::tick(uint64_t now_us) {
    if (!in_session_) return;
    if (!paused_ && (now_us - last_event_us_) / 1000 >= cfg_.pause_after_ms) {
        paused_ = true;  // silent mode-flip; the timeline just keeps counting
    }
    if ((now_us - last_activity_us_) / 1000 >= cfg_.idle_end_ms) {
        end_session(now_us);  // idle-timeout failsafe (capture spec §6)
    }
}

void SessionController::bookmark(uint64_t now_us) {
    ensure_session(now_us);
    CaptureRecord r{};
    r.type = RecType::Bookmark;
    r.t = stamp(now_us);
    push(r);
    last_activity_us_ = now_us;
}

void SessionController::grid_start(uint64_t now_us, uint16_t bpm, uint8_t subdiv,
                                   uint64_t downbeat_us) {
    ensure_session(now_us);
    CaptureRecord r{};
    r.type = RecType::GridStart;
    r.t = stamp(now_us);
    const uint64_t anchor = downbeat_us < session_start_us_ ? session_start_us_ : downbeat_us;
    r.u.grid = GridP{bpm, subdiv, (uint32_t)((anchor - session_start_us_) / 1000)};
    push(r);
    grid_open_ = true;  // re-declaration = §5 param-change rule; brain folds it
    last_activity_us_ = now_us;
}

bool SessionController::grid_end(uint64_t now_us) {
    if (!in_session_ || !grid_open_) return false;
    CaptureRecord r{};
    r.type = RecType::GridEnd;
    r.t = stamp(now_us);
    push(r);
    grid_open_ = false;
    last_activity_us_ = now_us;
    return true;
}

void SessionController::enroll_start(uint64_t now_us, const char* profile_ref,
                                     uint16_t bpm, uint8_t subdiv, uint64_t downbeat_us) {
    ensure_session(now_us);
    if (enroll_open_) {
        enroll_end(now_us);  // tidy files: auto-close the previous span
    }
    CaptureRecord r{};
    r.type = RecType::EnrollStart;
    r.t = stamp(now_us);
    strncpy(r.u.enroll.profile_ref, profile_ref, kProfileRefMax);
    r.u.enroll.profile_ref[kProfileRefMax] = '\0';
    r.u.enroll.bpm = bpm;
    r.u.enroll.subdiv = subdiv;
    const uint64_t anchor = downbeat_us < session_start_us_ ? session_start_us_ : downbeat_us;
    r.u.enroll.downbeat_t = (uint32_t)((anchor - session_start_us_) / 1000);
    push(r);
    enroll_open_ = true;
    last_activity_us_ = now_us;
}

bool SessionController::enroll_end(uint64_t now_us) {
    if (!in_session_ || !enroll_open_) return false;
    CaptureRecord r{};
    r.type = RecType::EnrollEnd;
    r.t = stamp(now_us);
    push(r);
    enroll_open_ = false;
    last_activity_us_ = now_us;
    return true;
}

void SessionController::end_session(uint64_t now_us) {
    if (!in_session_) return;
    grid_end(now_us);
    enroll_end(now_us);

    CaptureRecord r{};
    r.type = RecType::SessionEnd;
    r.t = stamp(now_us);
    push(r);
    counters_.sessions_ended++;

    in_session_ = false;
    paused_ = false;
    grid_open_ = false;
    enroll_open_ = false;
    // cache values persist: the next session's t=0 full flush re-baselines
}

}  // namespace edrum
