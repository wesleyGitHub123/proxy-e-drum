// The control-plane vocabulary (capture spec §5's gesture grammar, made
// concrete): bookmark / grid start-end / enroll start-end / end session.
// Any controller — console, gesture, a future desktop/mobile app, a
// hardware button, BLE, a network API — constructs one of these and hands
// it to a ControlDispatcher (control_dispatch.h). Nothing here assumes
// *how* it got there (queue hop across tasks vs. a direct in-process call);
// that's a transport decision each controller/composition-root makes for
// itself. What's fixed is the vocabulary and the fact that exactly one
// dispatcher, called from exactly one execution context (the capture task,
// the single ring producer — capture spec §6), ever applies it to the
// session FSM, preserving total timeline order.
//
// Deliberately excluded: anything that isn't a session-control declaration.
// The synthetic burst load generator (Experiment 3) is a firmware self-test
// concern, not something a BLE controller or a mobile app should ever need
// to know exists, so it is not part of this vocabulary — it stays a
// composition-root-local message type (see src/app.h).
#pragma once

#include <stdint.h>

#include "edrum/records.h"

namespace edrum {

struct ControlMsg {
    enum class Op : uint8_t {
        Bookmark,
        GridStart,     // snapshot the running click
        GridEnd,
        GridToggle,    // start if closed, end if open (controller doesn't
                       // need to track grid state itself)
        EnrollStart,   // ref empty = no label supplied (anonymous)
        EnrollEnd,
        EnrollToggle,  // start (anonymous) if closed, end if open
        EndSession,
    };
    Op op = Op::Bookmark;
    char ref[kProfileRefMax + 1] = {0};  // EnrollStart only
};

// What happened, for the controller/console to render. The dispatcher does
// no I/O itself (engine-purity discipline: no print, no side channel beyond
// the FSM/gestures it was handed) — outcomes carry enough data for a caller
// to print a message without re-deriving FSM state.
enum class ControlOutcome : uint8_t {
    BookmarkAdded,
    GridOpened,
    GridRefusedNoClick,
    GridClosed,
    GridNoSpanOpen,
    EnrollOpened,
    EnrollRefusedNoClick,
    EnrollClosed,
    EnrollNoSpanOpen,
    SessionEnded,
};

struct ControlResult {
    ControlOutcome outcome;
    uint16_t bpm = 0;                    // *Opened only
    uint8_t subdiv = 0;                  // *Opened only
    char ref[kProfileRefMax + 1] = {0};  // EnrollOpened only; "" = anonymous
};

}  // namespace edrum
