#include "edrum/click_wave.h"

#include <math.h>

namespace edrum {

size_t synth_click(int16_t* out, size_t cap, uint32_t sample_rate, bool accent) {
    const float freq = accent ? 1760.0f : 1174.7f;  // A6 / D6-ish
    const float amp = accent ? 0.70f : 0.55f;
    const float dur_s = 0.025f;
    size_t n = (size_t)(dur_s * (float)sample_rate);
    if (n > cap) n = cap;
    const float k = 180.0f;  // decay: ~ -40 dB by 25 ms
    const float two_pi_f = 6.2831853f * freq;
    for (size_t i = 0; i < n; ++i) {
        const float t = (float)i / (float)sample_rate;
        const float v = amp * expf(-k * t) * sinf(two_pi_f * t);
        out[i] = (int16_t)(v * 32767.0f);
    }
    return n;
}

}  // namespace edrum
