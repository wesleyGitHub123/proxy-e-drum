// Storage task (core 1) — the single ring consumer AND the single SD owner.
// Pops records, hands them to LogWriter (serialize -> append -> sync
// policy), checkpoints the wall clock at session close, and — when the
// console has handed the line over (decision 1) — pumps the sync protocol.
//
// Ordering is the fail-safety: the ring drain runs unconditionally FIRST
// every iteration (capture always wins), and one bounded link chunk is
// serviced after it, so a transfer can never starve the log or smear a
// timestamp (stamps happen on the other side of the ring, on core 0).
// Running the sync service here is also what keeps SD access single-
// threaded by construction — the same single-owner discipline as the ring.
//
// A slow SD moment shows up here as backlog (ring high-water) and stall
// counters — never as capture jitter. That separation is the whole point
// (capture spec §2).
#include "app.h"

namespace {

// SyncService responses -> framed bytes on the link.
struct LinkFrameSink : edrum::FrameSink {
    void emit(const uint8_t* payload, size_t len) override {
        uint8_t frame[edrum::kFrameEncodedMax];
        const size_t n = edrum::frame_encode(payload, len, frame, sizeof(frame));
        if (n > 0) app.link->write(frame, n);
    }
};

LinkFrameSink link_sink;
edrum::FrameAccumulator accum;
uint64_t last_rx_us = 0;
bool in_sync = false;

void pump_sync() {
    const uint64_t now = app.clock.now_us();
    if (!in_sync) {  // entering framed mode: clean slate
        accum.reset();
        last_rx_us = now;
        in_sync = true;
    }

    // Refresh the service's view of the world (fail-soft decision 5: a dead
    // card answers ERR StorageFailed on the wire, never a hang).
    app.sync->set_storage_ok(app.storage_ok && !app.writer->failed());
    app.sync->set_open_session(app.writer->open_name());

    uint8_t chunk[appcfg::kSyncReadChunk];
    const int n = app.link->read(chunk, sizeof(chunk));
    if (n > 0) last_rx_us = now;

    bool over = false;
    for (int i = 0; i < n; ++i) {
        uint8_t payload[edrum::kFramePayloadMax];
        const size_t plen = accum.push(chunk[i], payload, sizeof(payload));
        if (plen == 0) continue;
        const edrum::SyncService::Result r = app.sync->handle(payload, plen, link_sink);
        if (r.time_requested) {
            app.wall.set_time(r.host_time);  // every sync heals session dating (ADR-5)
        }
        if (r.session_over) over = true;
    }

    if (over || (now - last_rx_us) / 1000 > appcfg::kSyncIdleTimeoutMs) {
        app.sync_mode = false;  // console_poll notices and resumes
    }
}

}  // namespace

void storage_task(void*) {
    edrum::CaptureRecord rec;
    while (true) {
        bool ended = false;
        while (app.ring.pop(rec)) {
            app.writer->handle(rec);
            if (rec.type == edrum::RecType::SessionEnd) ended = true;
        }
        app.writer->idle();  // time-based sync when quiet
        if (ended) {
            app.wall.checkpoint();
        }

        if (app.sync_mode) {
            pump_sync();
        } else {
            in_sync = false;
        }

        vTaskDelay(pdMS_TO_TICKS(appcfg::kStoragePollMs));
    }
}
