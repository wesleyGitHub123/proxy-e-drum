// Click block renderer — mixes scheduled click edges into fixed mono PCM
// blocks. Pure and natively testable: the click task feeds it real time and
// pipes blocks to I2S; tests feed it scripted time and inspect samples.
//
// Timing model (capture spec §4 + architecture plan F4): the I2S DMA paces
// the loop (write() blocks), but every block DECIDES from IClock-time which
// edges fall inside its window — so sample-clock vs esp_timer drift self-
// corrects to ±1 block (1 ms at 48 kHz / 48-frame blocks). The remaining
// pipeline latency is constant and lands in calibration_offset_ms.
//
// Concurrency split (decision 4 — sub-ms interrupt windows on core 0): the
// scheduler is spinlock-shared (console starts/stops it, declarations
// snapshot it), so only plan_block() — tiny integer edge decisions — runs
// inside the click_mux critical section. All sample work (voice bookkeeping
// + mixing into the block buffer) happens in mix_block(), outside the lock:
// voice state belongs to the click task alone. render_block() composes both
// for single-threaded callers and tests.
//
// Gain (decision 2): the output level that sits right in the TD-02's MIX IN
// is a *setup* property of this hardware chain, so it is a firmware config
// knob applied here at begin() — never brain-side data. Scaling the two
// pre-rendered voice buffers once costs nothing per sample.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edrum/click_sched.h"
#include "edrum/click_wave.h"
#include "edrum/counters.h"

namespace edrum {

class ClickRenderer {
public:
    static constexpr size_t kMaxVoice = 2048;  // 25 ms @ up to 48 kHz + slack

    // Edge decisions for one block, computed under the lock, consumed
    // outside it. Beat periods are >> block length at any sane bpm, so two
    // trigger slots cover every real case (extras are advanced + counted in
    // clicks_rendered but not re-triggered — same audible result).
    struct BlockPlan {
        bool running = false;
        uint8_t ntrig = 0;
        struct Trig {
            uint16_t offset;  // frame offset within the block
            uint8_t accent;
        } trig[2];
    };

    ClickRenderer(ClickScheduler& sched, Counters& counters)
        : sched_(sched), counters_(counters) {}

    // gain_pct 0-100 scales the synthesized voices once, at init.
    void begin(uint32_t sample_rate, uint8_t gain_pct = 100) {
        rate_ = sample_rate;
        normal_len_ = synth_click(normal_, kMaxVoice, sample_rate, false);
        accent_len_ = synth_click(accent_, kMaxVoice, sample_rate, true);
        if (gain_pct > 100) gain_pct = 100;
        for (size_t i = 0; i < normal_len_; ++i) {
            normal_[i] = (int16_t)((int32_t)normal_[i] * gain_pct / 100);
        }
        for (size_t i = 0; i < accent_len_; ++i) {
            accent_[i] = (int16_t)((int32_t)accent_[i] * gain_pct / 100);
        }
        active_ = false;
    }

    // LOCKED phase: read/advance the shared scheduler, decide which edges
    // land in [block_start_us, +nframes/rate). Integer math only.
    void plan_block(BlockPlan& out, size_t nframes, uint64_t block_start_us);

    // UNLOCKED phase: apply the plan — continue/trigger voices and mix
    // samples into `out` (the click task's dedicated block buffer).
    void mix_block(int16_t* out, size_t nframes, const BlockPlan& plan);

    // Convenience for single-threaded callers and tests.
    void render_block(int16_t* out, size_t nframes, uint64_t block_start_us) {
        BlockPlan plan;
        plan_block(plan, nframes, block_start_us);
        mix_block(out, nframes, plan);
    }

    // Click task calls this while idle (scheduler stopped) so a stale voice
    // never resumes when the click restarts.
    void quiet() { active_ = false; }

private:
    ClickScheduler& sched_;
    Counters& counters_;
    uint32_t rate_ = 48000;

    int16_t normal_[kMaxVoice];
    int16_t accent_[kMaxVoice];
    size_t normal_len_ = 0;
    size_t accent_len_ = 0;

    // one active voice (click length << beat period at any sane bpm);
    // touched only by mix_block/quiet — the click task's context.
    bool active_ = false;
    const int16_t* voice_ = nullptr;
    size_t voice_len_ = 0;
    size_t voice_pos_ = 0;
};

}  // namespace edrum
