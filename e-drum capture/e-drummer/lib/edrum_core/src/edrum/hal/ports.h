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

// Transport-agnostic byte pipe for the device<->brain link (capture spec
// §13). A byte pipe is the least common denominator every candidate
// transport offers (UART, USB-CDC, TCP, BLE characteristic pair) — which is
// what makes this port future-proof. Framing/CRC deliberately do NOT live
// in the adapters: they sit once in edrum_core (frame.h), so every
// transport inherits the identical, natively-tested wire discipline.
//
// read() is non-blocking (0 = nothing pending); write() may block briefly
// on the transport's TX path. Both are called from the storage task only —
// never from the capture edge.
struct IByteLink {
    virtual int read(uint8_t* buf, size_t cap) = 0;  // bytes read, or -1
    virtual bool write(const uint8_t* data, size_t len) = 0;
    virtual ~IByteLink() = default;
};

constexpr size_t kSessionNameMax = 40;  // "YYYYMMDDTHHMMSS_<id8>.jsonl" + slack

struct SessionInfo {
    char name[kSessionNameMax + 1];
    uint32_t size;
};

// Read side of the session archive — the sync protocol's view of storage
// (capture spec §13). Deliberately a SEPARATE port from IStorage: the
// writer's single-open-file append-only model is load-bearing for crash
// survivability and must not widen into a general filesystem API. All
// methods are called from the storage task (single SD owner by
// construction); implementations return -1 on storage failure (fail-soft:
// the service maps that to an explicit wire error, never a hang).
struct ISessionArchive {
    // Fill `out` with up to `cap` closed-or-open session entries (the
    // caller excludes the open one). Returns the count, or -1.
    virtual int list(SessionInfo* out, size_t cap) = 0;
    // Size in bytes, or -1 if absent/unreadable.
    virtual int64_t size_of(const char* name) = 0;
    // Read up to `len` bytes at `offset`. Returns bytes read (0 = at/past
    // EOF), or -1 on failure.
    virtual int read(const char* name, uint32_t offset, uint8_t* buf, uint32_t len) = 0;
    virtual ~ISessionArchive() = default;
};

// The running click's state, as needed by a grid/enroll declaration
// (capture spec §5: declarations snapshot the RUNNING click, not a
// requested one). Narrow on purpose: this is the one piece of hardware-
// adjacent state (the click scheduler, possibly lock-guarded) that control
// dispatch needs but must not reach for directly, so the dispatch logic
// stays portable/native-testable (edrum/control_dispatch.h).
struct IClickSnapshot {
    // Returns false if no click is running (nothing to snapshot); *bpm/
    // *subdiv/*downbeat_us are only written on true.
    virtual bool snapshot(uint64_t now_us, uint16_t* bpm, uint8_t* subdiv,
                          uint64_t* downbeat_us) = 0;
    virtual ~IClickSnapshot() = default;
};

}  // namespace hal
}  // namespace edrum
