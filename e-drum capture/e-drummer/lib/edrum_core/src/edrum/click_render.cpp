#include "edrum/click_render.h"

#include <string.h>

namespace edrum {

void ClickRenderer::plan_block(BlockPlan& out, size_t nframes, uint64_t block_start_us) {
    out.running = sched_.running();
    out.ntrig = 0;
    if (!out.running) return;

    const uint64_t block_end_us = block_start_us + (uint64_t)nframes * 1000000ULL / rate_;
    while (sched_.next_beat_us() < block_end_us) {
        const uint64_t edge = sched_.next_beat_us();
        size_t offset = 0;
        if (edge > block_start_us) {
            offset = (size_t)((edge - block_start_us) * rate_ / 1000000ULL);
            if (offset >= nframes) break;  // rounding: next block takes it
        } else {
            // late edge (start mid-block or a scheduling stall): render now
            const uint64_t late = block_start_us - edge;
            if ((uint32_t)late > counters_.click_late_us_max) {
                counters_.click_late_us_max = (uint32_t)late;
            }
        }

        if (out.ntrig < 2) {
            out.trig[out.ntrig].offset = (uint16_t)offset;
            out.trig[out.ntrig].accent = sched_.next_is_accent() ? 1 : 0;
            out.ntrig++;
        }
        sched_.advance();
        counters_.clicks_rendered++;
    }
}

void ClickRenderer::mix_block(int16_t* out, size_t nframes, const BlockPlan& plan) {
    memset(out, 0, nframes * sizeof(int16_t));
    if (!plan.running) {
        active_ = false;
        return;
    }

    // 1. Continue a voice that started in an earlier block.
    if (active_) {
        for (size_t i = 0; i < nframes && voice_pos_ < voice_len_; ++i, ++voice_pos_) {
            out[i] = voice_[voice_pos_];
        }
        if (voice_pos_ >= voice_len_) active_ = false;
    }

    // 2. Trigger the planned edges; a later voice takes over from its offset.
    for (uint8_t k = 0; k < plan.ntrig; ++k) {
        voice_ = plan.trig[k].accent ? accent_ : normal_;
        voice_len_ = plan.trig[k].accent ? accent_len_ : normal_len_;
        voice_pos_ = 0;
        active_ = true;
        for (size_t i = plan.trig[k].offset; i < nframes && voice_pos_ < voice_len_;
             ++i, ++voice_pos_) {
            out[i] = voice_[voice_pos_];
        }
        if (voice_pos_ >= voice_len_) active_ = false;
    }
}

}  // namespace edrum
