#include "edrum/click_render.h"

#include <string.h>

namespace edrum {

void ClickRenderer::render_block(int16_t* out, size_t nframes, uint64_t block_start_us) {
    memset(out, 0, nframes * sizeof(int16_t));
    if (!sched_.running()) {
        active_ = false;
        return;
    }

    // 1. Continue a voice that started in an earlier block.
    if (active_) {
        size_t i = 0;
        for (; i < nframes && voice_pos_ < voice_len_; ++i, ++voice_pos_) {
            out[i] = voice_[voice_pos_];
        }
        if (voice_pos_ >= voice_len_) active_ = false;
    }

    // 2. Start every edge whose scheduled time falls inside this block.
    //    (Beat periods are >> block length, so at most one in practice; a
    //    later voice simply takes over the samples from its offset.)
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

        voice_ = sched_.next_is_accent() ? accent_ : normal_;
        voice_len_ = sched_.next_is_accent() ? accent_len_ : normal_len_;
        voice_pos_ = 0;
        active_ = true;
        sched_.advance();
        counters_.clicks_rendered++;

        for (size_t i = offset; i < nframes && voice_pos_ < voice_len_; ++i, ++voice_pos_) {
            out[i] = voice_[voice_pos_];
        }
        if (voice_pos_ >= voice_len_) active_ = false;
    }
}

}  // namespace edrum
