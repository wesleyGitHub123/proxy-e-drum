// Date/time formatting and parsing for the capture half — pure integer math.
// ISO 8601 subset matching what the brain writes/reads: seconds precision,
// explicit UTC offset (edrum/io/clock.py isoformat(timespec="seconds")), and
// the session filename convention (phase0-plan micro-decision 5).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace edrum {

struct DateTime {
    int16_t year = 1970;   // full year
    uint8_t month = 1;     // 1-12
    uint8_t day = 1;       // 1-31
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    int16_t tz_offset_min = 0;  // minutes east of UTC (e.g. +480 = +08:00)
};

// "2026-07-06T12:00:00+08:00" (always 25 chars). Returns length or 0 if cap
// is too small. The offset sign renders '+' for zero.
size_t format_iso(const DateTime& dt, char* out, size_t cap);

// "YYYYMMDDTHHMMSS_<id8>.jsonl" — micro-decision 5. id8 = first 8 chars of
// the session id. Returns length or 0 if cap too small.
size_t session_filename(const DateTime& dt, const char* session_id, char* out, size_t cap);

// Strict parse of "YYYY-MM-DDTHH:MM:SS[(+|-)HH:MM|Z]". Missing offset = UTC.
// Rejects out-of-range fields. Returns false on any deviation.
bool parse_iso(const char* s, DateTime& out);

// Civil-calendar <-> Unix epoch seconds (proleptic Gregorian; Hinnant's
// algorithm). tz_offset_min is applied so DateTime is the *local* rendering.
int64_t datetime_to_epoch(const DateTime& dt);
void epoch_to_datetime(int64_t epoch_sec, int16_t tz_offset_min, DateTime& out);

// 16 random bytes -> lowercase uuid4-style 32-hex-char id (version/variant
// bits forced, mirroring uuid.uuid4().hex). out must hold 33 bytes.
void uuid4_hex(const uint8_t bytes[16], char out[33]);

}  // namespace edrum
