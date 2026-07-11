// Click voice synthesis — a short exponentially-decaying sine burst, two
// pitches (accented downbeat vs regular beat). Pure; rendered once at init
// into RAM buffers, then copied by the renderer.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace edrum {

// Fills `out` with up to `cap` mono int16 samples; returns the click length
// in samples (~25 ms at `sample_rate`). accent = higher pitch, slightly hotter.
size_t synth_click(int16_t* out, size_t cap, uint32_t sample_rate, bool accent);

}  // namespace edrum
