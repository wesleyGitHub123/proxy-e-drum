// Serial console command grammar (Communications subsystem). PARSING ONLY —
// pure and natively tested; execution lives in the app's console task. The
// console is the laptop-phase control surface (set time, drive the click,
// declare spans, read health counters, run the Experiment-3 burst).
#pragma once

#include <stdint.h>

#include "edrum/records.h"
#include "edrum/timefmt.h"

namespace edrum {

struct Command {
    enum class Kind : uint8_t {
        None,        // empty line
        Invalid,     // unparseable (message in `error`)
        Help,
        Stats,       // dump counters
        Time,        // show current wall time
        SetTime,     // settime 2026-07-10T21:30:00+08:00
        ClickStart,  // click 120 [4]
        ClickStop,   // click off
        GridStart,   // grid start
        GridEnd,     // grid end
        Bookmark,    // bookmark
        EnrollStart, // enroll basic-rock
        EnrollEnd,   // enroll end
        EndSession,  // end
        Burst,       // burst 2000 [500]   (count, events/sec)
    };

    Kind kind = Kind::None;
    uint16_t bpm = 0;
    uint8_t subdiv = 0;
    char ref[kProfileRefMax + 1] = {0};
    DateTime dt;
    uint32_t burst_count = 0;
    uint16_t burst_hz = 0;
    const char* error = nullptr;  // static string when kind == Invalid
};

Command parse_command(const char* line);

// One static help text (kept here so the grammar and its doc can't drift).
const char* console_help();

}  // namespace edrum
