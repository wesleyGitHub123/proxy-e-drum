#include "edrum/logwriter.h"

#include <stdio.h>
#include <string.h>

#include "edrum/timefmt.h"

namespace edrum {

void LogWriter::stalled_call_end(uint64_t t0) {
    const uint64_t stall = clock_.now_us() - t0;
    if (stall > counters_.write_stall_max_us) {
        counters_.write_stall_max_us = (uint32_t)stall;
    }
    if (stall > 50000) counters_.write_stall_over_50ms++;
}

void LogWriter::open_session(const SessionStartP& s) {
    if (open_) {
        // FSM guarantees end-before-start; tolerate a foreign producer bug.
        close_session();
    }

    DateTime dt;
    if (!parse_iso(s.start_iso, dt)) {
        counters_.storage_write_errors++;
        return;
    }
    char name[48];
    if (session_filename(dt, s.session_id, name, sizeof(name)) == 0) {
        counters_.storage_write_errors++;
        return;
    }
    char path[80];
    const int n = snprintf(path, sizeof(path), "%s/%s", cfg_.dir, name);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        counters_.storage_write_errors++;
        return;
    }

    uint64_t t0 = stalled_call_begin();
    const bool created = storage_.create(path);
    stalled_call_end(t0);
    if (!created) {
        counters_.storage_write_errors++;
        return;  // records until the next SessionStart will be dropped+counted
    }

    const size_t len = meta_line(s, meta_, line_, sizeof(line_));
    if (len == 0) {
        counters_.serialize_errors++;
        storage_.close();
        return;
    }
    open_ = true;
    unsynced_bytes_ = 0;
    last_sync_us_ = clock_.now_us();
    last_t_ = 0;
    counters_.files_opened++;
    append_line(line_, len);
    sync_if_due(true);  // line 1 durable immediately
}

void LogWriter::close_session() {
    if (!open_) return;
    sync_if_due(true);
    uint64_t t0 = stalled_call_begin();
    storage_.close();
    stalled_call_end(t0);
    open_ = false;
}

void LogWriter::append_line(const char* buf, size_t len) {
    uint64_t t0 = stalled_call_begin();
    const bool ok = storage_.append(reinterpret_cast<const uint8_t*>(buf), len);
    stalled_call_end(t0);
    if (!ok) {
        counters_.storage_write_errors++;
        return;
    }
    counters_.storage_lines++;
    counters_.storage_bytes += (uint32_t)len;
    unsynced_bytes_ += (uint32_t)len;
}

void LogWriter::sync_if_due(bool force) {
    if (!open_ || unsynced_bytes_ == 0) return;  // nothing pending to make durable
    const uint64_t now = clock_.now_us();
    const bool bytes_due = unsynced_bytes_ >= cfg_.sync_every_bytes;
    const bool time_due = (now - last_sync_us_) / 1000 >= cfg_.sync_every_ms;
    if (!force && !bytes_due && !time_due) return;

    uint64_t t0 = stalled_call_begin();
    const bool ok = storage_.sync();
    stalled_call_end(t0);
    if (!ok) {
        counters_.storage_write_errors++;
        return;
    }
    counters_.storage_syncs++;
    unsynced_bytes_ = 0;
    last_sync_us_ = now;
}

void LogWriter::handle(const CaptureRecord& rec) {
    if (rec.type == RecType::SessionStart) {
        open_session(rec.u.session_start);
        return;
    }
    if (!open_) {
        counters_.storage_write_errors++;  // record with no session file
        return;
    }

    // Producer-side bug trap (mirror of brain SessionWriter's monotonic assert
    // — the firmware counts instead of crashing; t is never modified).
    if (rec.t < last_t_) {
        counters_.storage_write_errors++;
    } else {
        last_t_ = rec.t;
    }

    const size_t len = record_line(rec, line_, sizeof(line_));
    if (len == 0) {
        counters_.serialize_errors++;
        return;
    }
    append_line(line_, len);

    if (rec.type == RecType::SessionEnd) {
        close_session();
    } else {
        sync_if_due(false);
    }
}

void LogWriter::idle() {
    sync_if_due(false);
}

}  // namespace edrum
