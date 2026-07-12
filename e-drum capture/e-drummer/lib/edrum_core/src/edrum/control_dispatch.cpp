#include "edrum/control_dispatch.h"

#include <string.h>

namespace edrum {

ControlResult ControlDispatcher::start_grid(uint64_t now_us) {
    ControlResult r{};
    uint16_t bpm;
    uint8_t subdiv;
    uint64_t downbeat_us;
    if (!click_.snapshot(now_us, &bpm, &subdiv, &downbeat_us)) {
        r.outcome = ControlOutcome::GridRefusedNoClick;
        return r;
    }
    fsm_.grid_start(now_us, bpm, subdiv, downbeat_us);
    r.outcome = ControlOutcome::GridOpened;
    r.bpm = bpm;
    r.subdiv = subdiv;
    return r;
}

ControlResult ControlDispatcher::start_enroll(uint64_t now_us, const char* ref) {
    ControlResult r{};
    uint16_t bpm;
    uint8_t subdiv;
    uint64_t downbeat_us;
    if (!click_.snapshot(now_us, &bpm, &subdiv, &downbeat_us)) {
        r.outcome = ControlOutcome::EnrollRefusedNoClick;
        return r;
    }
    fsm_.enroll_start(now_us, ref, bpm, subdiv, downbeat_us);
    r.outcome = ControlOutcome::EnrollOpened;
    r.bpm = bpm;
    r.subdiv = subdiv;
    strncpy(r.ref, ref, kProfileRefMax);
    r.ref[kProfileRefMax] = '\0';
    return r;
}

ControlResult ControlDispatcher::dispatch(const ControlMsg& msg, uint64_t now_us) {
    ControlResult r{};
    switch (msg.op) {
        case ControlMsg::Op::Bookmark:
            fsm_.bookmark(now_us);
            r.outcome = ControlOutcome::BookmarkAdded;
            return r;

        case ControlMsg::Op::GridStart:
            return start_grid(now_us);

        case ControlMsg::Op::GridEnd:
            r.outcome = fsm_.grid_end(now_us) ? ControlOutcome::GridClosed
                                              : ControlOutcome::GridNoSpanOpen;
            return r;

        case ControlMsg::Op::GridToggle:
            if (fsm_.grid_open()) {
                fsm_.grid_end(now_us);
                r.outcome = ControlOutcome::GridClosed;
                return r;
            }
            return start_grid(now_us);

        case ControlMsg::Op::EnrollStart:
            return start_enroll(now_us, msg.ref);

        case ControlMsg::Op::EnrollEnd:
            r.outcome = fsm_.enroll_end(now_us) ? ControlOutcome::EnrollClosed
                                                : ControlOutcome::EnrollNoSpanOpen;
            return r;

        case ControlMsg::Op::EnrollToggle:
            if (fsm_.enroll_open()) {
                fsm_.enroll_end(now_us);
                r.outcome = ControlOutcome::EnrollClosed;
                return r;
            }
            // A toggle-triggered start carries no provenance to attach — the
            // controller (gesture, hardware button) has no label to offer,
            // so this is always anonymous. A controller that DOES have a
            // label uses EnrollStart directly instead of the toggle.
            return start_enroll(now_us, "");

        case ControlMsg::Op::EndSession:
            fsm_.end_session(now_us);
            gestures_.reset();
            r.outcome = ControlOutcome::SessionEnded;
            return r;
    }
    return r;  // unreachable; keeps -Wreturn-type quiet across toolchains
}

}  // namespace edrum
