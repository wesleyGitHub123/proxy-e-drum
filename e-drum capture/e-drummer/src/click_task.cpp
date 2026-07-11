// Click task (core 0, highest priority, tiny work) — renders the click
// schedule into 1 ms blocks and feeds I2S. i2s write() blocking on DMA is
// the pacing; every block re-reads IClock so esp_timer vs sample-clock
// drift self-corrects to ±1 block (see click_render.h).
//
// The schedule object is shared under app.click_mux: console starts/stops
// it, the capture task snapshots it for declarations, this task renders it.
#include "app.h"

void click_task(void*) {
    int16_t block[appcfg::kClickBlockFrames];

    while (true) {
        bool running;
        taskENTER_CRITICAL(&app.click_mux);
        running = app.click_sched.running();
        if (running) {
            app.click_render->render_block(block, appcfg::kClickBlockFrames,
                                           app.clock.now_us());
        }
        taskEXIT_CRITICAL(&app.click_mux);

        if (running && app.audio_ok) {
            app.audio->write(block, appcfg::kClickBlockFrames);  // blocks on DMA
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));  // idle; underrun auto-clears to silence
        }
    }
}
