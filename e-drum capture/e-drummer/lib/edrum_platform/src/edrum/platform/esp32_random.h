// IRandom adapter: esp_random (hardware RNG; RF subsystem seeds it, and
// bootloader entropy covers the no-RF case well enough for session ids).
#pragma once

extern "C" {
#include "esp_random.h"
}

#include "edrum/hal/ports.h"

namespace edrum {
namespace platform {

class Esp32Random : public hal::IRandom {
public:
    void fill(uint8_t* buf, size_t len) override { esp_fill_random(buf, len); }
};

}  // namespace platform
}  // namespace edrum
