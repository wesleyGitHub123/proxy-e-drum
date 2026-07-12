// Gesture surface (capture spec §5, DECISION 9c resolved for the box:
// detection lives in firmware, as a pure state machine over the event
// stream). The grammar is built from kick+crash CHORDS — both trigger pads
// within a tight window — repeated with short gaps, deliberately
// un-playable by accident (Jamcorder's black-keys-twice precedent):
//
//   chord x2      ->  Bookmark
//   chord x3      ->  GridToggle    (declare/end a grade span against the running click)
//   chord x4(+)   ->  EnrollToggle  (declare/end an enrollment span; always
//                                    anonymous — a gesture has no label to
//                                    offer, so this is the zero-friction
//                                    path; naming happens brain/UI-side)
//
// Sessions need no start gesture (they auto-start on the first hit) and end
// via the idle failsafe or console — so the gesture budget is spent on the
// three things a drummer wants mid-flow without reaching for a keyboard.
//
// Emission happens when the sequence times out (`seq_gap_ms` after the last
// chord) so e.g. a 3-chord run is not mistaken for 2 — poll() drives that
// from the capture task's tick.
#pragma once

#include <stdint.h>

namespace edrum {

class GestureDetector {
public:
    struct Cfg {
        bool enabled = true;
        uint8_t note_a = 36;         // kick (TD-02)
        uint8_t note_b = 49;         // crash 1 bow (TD-02)
        uint8_t min_velocity = 30;   // intent filter: taps don't trigger
        uint16_t chord_window_ms = 120;
        uint16_t seq_gap_ms = 700;   // max gap between chords; also emit delay
    };

    enum class Action : uint8_t { None, Bookmark, GridToggle, EnrollToggle };

    explicit GestureDetector(const Cfg& cfg) : cfg_(cfg) {}

    // Feed every performance event (session-t ms). Cheap; capture-task path.
    void on_event(uint32_t t, uint8_t note, uint8_t velocity);

    // Call periodically; returns the action when a chord sequence closes.
    Action poll(uint32_t t);

    // A session ended or config changed: forget in-flight state.
    void reset();

private:
    Cfg cfg_;
    uint32_t a_t_ = 0;
    uint32_t b_t_ = 0;
    bool a_seen_ = false;
    bool b_seen_ = false;
    uint8_t chord_count_ = 0;
    uint32_t last_chord_t_ = 0;
};

}  // namespace edrum
