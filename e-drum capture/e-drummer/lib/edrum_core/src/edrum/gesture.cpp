#include "edrum/gesture.h"

namespace edrum {

void GestureDetector::reset() {
    a_seen_ = b_seen_ = false;
    chord_count_ = 0;
}

void GestureDetector::on_event(uint32_t t, uint8_t note, uint8_t velocity) {
    if (!cfg_.enabled || velocity < cfg_.min_velocity) return;

    if (note == cfg_.note_a) {
        a_seen_ = true;
        a_t_ = t;
    } else if (note == cfg_.note_b) {
        b_seen_ = true;
        b_t_ = t;
    } else {
        return;  // other pads don't disturb a sequence in progress
    }

    if (a_seen_ && b_seen_) {
        const uint32_t gap = a_t_ > b_t_ ? a_t_ - b_t_ : b_t_ - a_t_;
        if (gap <= cfg_.chord_window_ms) {
            // a chord — but only count it as part of a sequence if it came
            // soon enough after the previous one
            if (chord_count_ > 0 && t - last_chord_t_ > cfg_.seq_gap_ms) {
                chord_count_ = 0;  // stale sequence; this chord starts anew
            }
            ++chord_count_;
            last_chord_t_ = t;
            a_seen_ = b_seen_ = false;
        }
    }
}

GestureDetector::Action GestureDetector::poll(uint32_t t) {
    if (!cfg_.enabled || chord_count_ == 0) return Action::None;
    if (t - last_chord_t_ <= cfg_.seq_gap_ms) return Action::None;  // may grow

    const uint8_t n = chord_count_;
    chord_count_ = 0;
    a_seen_ = b_seen_ = false;
    if (n == 2) return Action::Bookmark;
    if (n >= 3) return Action::GridToggle;
    return Action::None;  // single chord: just playing
}

}  // namespace edrum
