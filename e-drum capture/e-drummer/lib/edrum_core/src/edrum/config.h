// Device configuration — read from `/config.txt` on the SD card at boot
// (Configuration subsystem). Deliberately `key = value` lines, not JSON: the
// canonical-JSON emitter in serialize.cpp is for the corpus contract; config
// wants the simplest thing a human can edit on a card reader.
//
// Everything here is a *hint or policy knob*, never data the brain can't
// re-derive: kit_profile_id is the reassignable pointer (capture spec §7),
// calibration is per-session meta (capture spec §4), and the rest tunes
// firmware behavior only.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edrum/records.h"

namespace edrum {

struct Config {
    // meta fields (capture spec §7, §4)
    char user_id[kNameMax + 1];
    char kit_profile_id[kNameMax + 1];  // "" => null in meta
    bool has_calibration;
    int32_t calibration_offset_ms;

    // session dating (capture spec §6; ADR-5)
    int16_t tz_offset_min;

    // click defaults (capture spec §4)
    uint16_t click_bpm;
    uint8_t click_subdiv;

    // session boundaries (capture spec §6, Jamcorder-derived)
    uint32_t pause_after_ms;  // event silence before ctrl-caching starts
    uint32_t idle_end_ms;     // activity silence before session_end failsafe

    // gesture surface (capture spec §5; kick=36, crash=49 in the TD-02 map)
    bool gesture_enable;
    uint8_t gesture_note_a;
    uint8_t gesture_note_b;

    Config()
        : user_id{'l', 'o', 'c', 'a', 'l', '\0'},
          kit_profile_id{'t', 'd', '0', '2', 'k', '\0'},
          has_calibration(false),
          calibration_offset_ms(0),
          tz_offset_min(480),
          click_bpm(120),
          click_subdiv(4),
          pause_after_ms(3000),
          idle_end_ms(300000),
          gesture_enable(true),
          gesture_note_a(36),
          gesture_note_b(49) {}
};

struct ConfigParseStats {
    uint16_t applied = 0;
    uint16_t unknown = 0;
    uint16_t invalid = 0;
};

// Parse `text` (len bytes) of `key = value` lines ('#' comments, blank lines
// ok) into `io`, which arrives pre-loaded with defaults. Unknown keys are
// counted, never fatal (config evolves like the schema: additively).
ConfigParseStats parse_config(const char* text, size_t len, Config& io);

}  // namespace edrum
