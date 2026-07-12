// Translates ControlMsg -> SessionController calls — the one place a
// ControlMsg gets interpreted. SessionController itself never sees a
// ControlMsg and never knows a controller exists (source-agnostic FSM,
// architecture review "control path" decision): it keeps its own typed
// method API (bookmark(t), grid_start(t,bpm,subdiv,downbeat), ...), which is
// what let it stay trivially native-testable with plain arguments before
// this type existed and keeps it that way now.
//
// This class is the piece that used to live untested in capture_task.cpp's
// handle_control() — moved here so control routing (including the
// click-snapshot-refusal path) gets native coverage instead of only being
// reachable on hardware.
#pragma once

#include <stdint.h>

#include "edrum/control_msg.h"
#include "edrum/gesture.h"
#include "edrum/hal/ports.h"
#include "edrum/session_fsm.h"

namespace edrum {

class ControlDispatcher {
public:
    ControlDispatcher(SessionController& fsm, hal::IClickSnapshot& click,
                      GestureDetector& gestures)
        : fsm_(fsm), click_(click), gestures_(gestures) {}

    ControlResult dispatch(const ControlMsg& msg, uint64_t now_us);

private:
    ControlResult start_grid(uint64_t now_us);
    ControlResult start_enroll(uint64_t now_us, const char* ref);

    SessionController& fsm_;
    hal::IClickSnapshot& click_;
    GestureDetector& gestures_;
};

}  // namespace edrum
