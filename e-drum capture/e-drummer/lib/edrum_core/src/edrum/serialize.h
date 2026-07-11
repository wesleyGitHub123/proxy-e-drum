// Canonical line serialization — the firmware side of the session-file
// contract (brain spec §3; phase0-plan micro-decision 6, FROZEN):
//   UTF-8, LF line endings, compact separators, deterministic per-type key
//   order, integers where the schema says integer, ctrl msg keys = "type"
//   first then sorted alphabetically (mido-style names).
//
// This module must produce bytes IDENTICAL to edrum.engine.records.to_line()
// for every record the firmware emits. The golden-fixture conformance test
// (test/native/test_conformance) is the executable proof.
//
// v1 firmware restriction (documented): all numerics emitted are integers
// (bpm, calibration_offset_ms) — the schema permits floats, but emitting
// int-only sidesteps cross-language float formatting divergence entirely.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edrum/records.h"

namespace edrum {

// Static meta fields (from config) + per-session fields (from the
// SessionStart marker) -> the meta line.
struct MetaStatic {
    char kit_profile_id[kNameMax + 1];  // "" => null
    char user_id[kNameMax + 1];
    bool has_calibration;
    int32_t calibration_offset_ms;
};

// Longest legal line: ctrl sysex with 48 data bytes (~"data":[255,...] ≈ 250B)
// or a maximal meta (~230B). 512 covers everything with margin.
constexpr size_t kMaxLine = 512;

// Serialize the meta line (line 1). Returns byte length (incl. trailing \n)
// or 0 if cap is too small.
size_t meta_line(const SessionStartP& s, const MetaStatic& ms, char* out, size_t cap);

// Serialize any non-marker record to its canonical line (incl. trailing \n).
// Returns 0 for RecType::SessionStart (markers are not lines) or on overflow.
size_t record_line(const CaptureRecord& rec, char* out, size_t cap);

// JSON string escaping used above, exposed for tests: mirrors Python
// json.dumps(ensure_ascii=False) — escapes '"', '\\', and C0 controls
// (\b \t \n \f \r named; others \u00xx lowercase hex); UTF-8 passes through.
size_t json_escape(const char* in, char* out, size_t cap);

}  // namespace edrum
