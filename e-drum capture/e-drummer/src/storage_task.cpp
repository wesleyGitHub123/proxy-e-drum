// Storage task (core 1) — the single ring consumer. Pops records, hands
// them to LogWriter (serialize -> append -> sync policy), and checkpoints
// the wall clock at session close so post-power-loss dating stays ordered.
//
// A slow SD moment shows up here as backlog (ring high-water) and stall
// counters — never as capture jitter. That separation is the whole point
// (capture spec §2).
#include "app.h"

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
        vTaskDelay(pdMS_TO_TICKS(appcfg::kStoragePollMs));
    }
}
