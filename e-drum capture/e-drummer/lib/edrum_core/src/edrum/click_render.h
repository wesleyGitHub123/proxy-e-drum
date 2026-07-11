// Click block renderer — mixes scheduled click edges into fixed mono PCM
// blocks. Pure and natively testable: the click task feeds it real time and
// pipes blocks to I2S; tests feed it scripted time and inspect samples.
//
// Timing model (capture spec §4 + architecture plan F4): the I2S DMA paces
// the loop (write() blocks), but every block DECIDES from IClock-time which
// edges fall inside its window — so sample-clock vs esp_timer drift self-
// corrects to ±1 block (1 ms at 48 kHz / 48-frame blocks). The remaining
// pipeline latency is constant and lands in calibration_offset_ms.
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

    ClickRenderer(ClickScheduler& sched, Counters& counters)
        : sched_(sched), counters_(counters) {}

    void begin(uint32_t sample_rate) {
        rate_ = sample_rate;
        normal_len_ = synth_click(normal_, kMaxVoice, sample_rate, false);
        accent_len_ = synth_click(accent_, kMaxVoice, sample_rate, true);
        active_ = false;
    }

    // Render `nframes` mono samples covering [block_start_us, +nframes/rate).
    void render_block(int16_t* out, size_t nframes, uint64_t block_start_us);

private:
    ClickScheduler& sched_;
    Counters& counters_;
    uint32_t rate_ = 48000;

    int16_t normal_[kMaxVoice];
    int16_t accent_[kMaxVoice];
    size_t normal_len_ = 0;
    size_t accent_len_ = 0;

    // one active voice (click length << beat period at any sane bpm)
    bool active_ = false;
    const int16_t* voice_ = nullptr;
    size_t voice_len_ = 0;
    size_t voice_pos_ = 0;
};

}  // namespace edrum
