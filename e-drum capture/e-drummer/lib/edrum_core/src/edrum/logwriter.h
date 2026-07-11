// The Storage subsystem's brain: consume CaptureRecords (drained from the
// ring by the storage task) and drive the append-only session log through
// the IStorage port (capture spec §6; brain spec §3).
//
//   * SessionStart marker  -> exclusive-create sessions/<name>.jsonl, write
//     the meta line, sync it (line 1 durable immediately)
//   * every other record   -> canonical line append
//   * SessionEnd           -> append, sync, close
//
// The writer only ever appends — never seeks, never rewrites line 1. Crash
// recovery is the READER's job (truncate to last complete line, brain
// logfile.py); after power loss the file is valid up to the last synced
// line by construction.
//
// Sync policy (Experiment 3 knobs): sync when either `sync_every_bytes`
// unsynced bytes accumulate or `sync_every_ms` elapse with unsynced data.
// Storage stalls are measured here (write_stall_* counters) — they can
// never smear a timestamp because stamps happen on the other side of the
// ring (capture spec §2/§3).
#pragma once

#include <stdint.h>

#include "edrum/counters.h"
#include "edrum/hal/ports.h"
#include "edrum/records.h"
#include "edrum/serialize.h"

namespace edrum {

class LogWriter {
public:
    struct Cfg {
        const char* dir;           // e.g. "/sessions" (no trailing slash)
        uint32_t sync_every_bytes;
        uint32_t sync_every_ms;
    };

    LogWriter(const Cfg& cfg, hal::IStorage& storage, hal::IClock& clock,
              const MetaStatic& meta, Counters& counters)
        : cfg_(cfg), storage_(storage), clock_(clock), meta_(meta), counters_(counters) {}

    // Consume one record popped from the ring.
    void handle(const CaptureRecord& rec);

    // Call periodically (ring empty) so the time-based sync policy holds.
    void idle();

    bool file_open() const { return open_; }

private:
    void open_session(const SessionStartP& s);
    void close_session();
    void append_line(const char* buf, size_t len);
    void sync_if_due(bool force);
    uint64_t stalled_call_begin() { return clock_.now_us(); }
    void stalled_call_end(uint64_t t0);

    Cfg cfg_;
    hal::IStorage& storage_;
    hal::IClock& clock_;
    MetaStatic meta_;
    Counters& counters_;

    bool open_ = false;
    uint32_t unsynced_bytes_ = 0;
    uint64_t last_sync_us_ = 0;
    uint32_t last_t_ = 0;
    char line_[kMaxLine];
};

}  // namespace edrum
