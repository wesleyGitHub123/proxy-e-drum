// IClock adapter: esp_timer — 64-bit monotonic microseconds since boot.
// The ONE clock (capture spec §3 Rule 1): the same instance is handed to the
// capture stamp path and the click scheduler by the composition root.
#pragma once

extern "C" {
#include "esp_timer.h"
}

#include "edrum/hal/ports.h"

namespace edrum {
namespace platform {

class Esp32Clock : public hal::IClock {
public:
    uint64_t now_us() override { return (uint64_t)esp_timer_get_time(); }
};

}  // namespace platform
}  // namespace edrum
