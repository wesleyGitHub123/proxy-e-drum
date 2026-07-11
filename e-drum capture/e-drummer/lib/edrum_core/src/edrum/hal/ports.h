// The HAL ports — the narrow interfaces everything hardware-shaped hides
// behind (firmware architecture plan §2/§3). Core code depends on these;
// lib/edrum_platform implements them; native tests substitute fakes.
//
// IClock is the load-bearing one: the one-clock rule (capture spec §3 Rule 1)
// is enforced by handing the SAME instance to the capture stamp path and the
// click scheduler at composition time.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edrum/timefmt.h"

namespace edrum {
namespace hal {

// Monotonic microseconds since boot. Never pauses, never rebases.
struct IClock {
    virtual uint64_t now_us() = 0;
    virtual ~IClock() = default;
};

// Wall-clock time for session dating (capture spec §6; ADR-5 still open —
// the source behind this port is swappable: set-via-console now, RTC later).
// Returns false when no plausible wall time is available; callers must then
// fall back to an ordering-only timestamp.
struct IWallClock {
    virtual bool now(DateTime& out) = 0;
    virtual ~IWallClock() = default;
};

struct IRandom {
    virtual void fill(uint8_t* buf, size_t len) = 0;
    virtual ~IRandom() = default;
};

// Append-only storage with a single-open-file model (one session file is
// open at a time — capture spec §6). create() must be exclusive: refuse to
// clobber an existing file (files are never rewritten, brain spec §3).
struct IStorage {
    virtual bool exists(const char* path) = 0;
    // Read a whole small file (config). Returns byte count or -1 if absent.
    virtual int read_file(const char* path, char* buf, size_t cap) = 0;
    virtual bool create(const char* path) = 0;
    virtual bool append(const uint8_t* data, size_t len) = 0;
    virtual bool sync() = 0;
    virtual void close() = 0;
    virtual ~IStorage() = default;
};

// Mono 16-bit PCM sink for the click (capture spec §4). write() blocks until
// the frames are queued (DMA backpressure paces the click renderer).
struct IAudioOut {
    virtual bool begin(uint32_t sample_rate) = 0;
    virtual size_t write(const int16_t* frames, size_t nframes) = 0;
    virtual ~IAudioOut() = default;
};

}  // namespace hal
}  // namespace edrum
