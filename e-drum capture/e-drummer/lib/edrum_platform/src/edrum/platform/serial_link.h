// IByteLink adapter over an Arduino Stream — transport #1 of the device<->
// brain link (capture spec §13): the UART/USB bridge port the console
// already lives on, arbitrated modally (console `sync` command hands the
// line over; BYE or idle timeout hands it back — decision 1).
//
// Deliberately the least interesting transport: zero new hardware (ADR-4's
// two-port topology), proves the whole seam while pinning nothing. BLE /
// Wi-Fi / TCP become sibling adapters of the same port; framing/CRC live in
// edrum_core (frame.h), so every transport inherits the identical wire
// discipline.
#pragma once

#include <Arduino.h>

#include "edrum/hal/ports.h"

namespace edrum {
namespace platform {

class SerialLink : public hal::IByteLink {
public:
    explicit SerialLink(Stream& s) : s_(s) {}

    int read(uint8_t* buf, size_t cap) override {
        size_t n = 0;
        while (n < cap) {
            const int c = s_.read();
            if (c < 0) break;
            buf[n++] = (uint8_t)c;
        }
        return (int)n;
    }

    bool write(const uint8_t* data, size_t len) override {
        return s_.write(data, len) == len;  // may block briefly on TX (core 1)
    }

private:
    Stream& s_;
};

}  // namespace platform
}  // namespace edrum
