// Click schedule — pure beat-edge arithmetic on the one clock (capture spec
// §4: the device is the tempo authority; the grid is exact-by-construction
// because the schedule, not the heard audio, is what grading compares to).
//
// Integer math only: beat n = start + round(n * 60e6 / bpm) µs — no float,
// no cumulative drift (each edge is computed from the anchor, not the
// previous edge).
#pragma once

#include <stdint.h>

namespace edrum {

class ClickScheduler {
public:
    void start(uint64_t now_us, uint16_t bpm, uint8_t subdiv, uint8_t beats_per_bar = 4) {
        bpm_ = bpm;
        subdiv_ = subdiv;
        beats_per_bar_ = beats_per_bar == 0 ? 1 : beats_per_bar;
        start_us_ = now_us;
        next_n_ = 0;
        running_ = true;
    }

    void stop() { running_ = false; }
    bool running() const { return running_; }
    uint16_t bpm() const { return bpm_; }
    uint8_t subdiv() const { return subdiv_; }
    uint64_t start_us() const { return start_us_; }

    uint64_t beat_us(uint32_t n) const {
        return start_us_ + ((uint64_t)n * 60000000ULL + bpm_ / 2) / bpm_;
    }

    uint64_t next_beat_us() const { return beat_us(next_n_); }
    uint32_t next_beat_index() const { return next_n_; }
    bool next_is_accent() const { return next_n_ % beats_per_bar_ == 0; }
    void advance() { ++next_n_; }

    // Declaration anchor (grid_start/enroll_start downbeat): the first beat
    // edge at or after `now_us`.
    uint64_t next_edge_at_or_after(uint64_t now_us) const {
        if (!running_ || now_us <= start_us_) return start_us_;
        const uint64_t elapsed = now_us - start_us_;
        // smallest n with beat_us(n) >= now: n = ceil(elapsed * bpm / 60e6)
        const uint64_t n = (elapsed * bpm_ + 60000000ULL - 1) / 60000000ULL;
        return beat_us((uint32_t)n);
    }

private:
    bool running_ = false;
    uint16_t bpm_ = 120;
    uint8_t subdiv_ = 4;
    uint8_t beats_per_bar_ = 4;
    uint64_t start_us_ = 0;
    uint32_t next_n_ = 0;
};

}  // namespace edrum
