// Click task (core 0, highest priority, tiny work) — renders the click
// schedule into 1 ms blocks and feeds I2S. i2s write() blocking on DMA is
// the pacing; every block re-reads IClock so esp_timer vs sample-clock
// drift self-corrects to ±1 block (see click_render.h).
//
// Concurrency split (decision 4): only plan_block — tiny integer edge
// decisions on the shared scheduler — runs inside the click_mux critical
// section (console starts/stops the schedule, declarations snapshot it).
// All sample mixing happens outside the lock into this task's dedicated
// block buffer, so the core-0 interrupt-off window stays microseconds-
// scale regardless of block size.
#include "app.h"

void click_task(void*) {
    int16_t block[appcfg::kClickBlockFrames];  // dedicated mixing buffer
    edrum::ClickRenderer::BlockPlan plan;

    while (true) {
        taskENTER_CRITICAL(&app.click_mux);
        app.click_render->plan_block(plan, appcfg::kClickBlockFrames, app.clock.now_us());
        taskEXIT_CRITICAL(&app.click_mux);

        if (plan.running && app.audio_ok) {
            app.click_render->mix_block(block, appcfg::kClickBlockFrames, plan);
            app.audio->write(block, appcfg::kClickBlockFrames);  // blocks on DMA
        } else {
            app.click_render->quiet();  // never resume a stale voice on restart
            vTaskDelay(pdMS_TO_TICKS(20));  // idle; underrun auto-clears to silence
        }
    }
}
