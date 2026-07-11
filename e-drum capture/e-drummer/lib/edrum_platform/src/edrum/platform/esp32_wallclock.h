// IWallClock adapter: ESP32 system time + an NVS checkpoint (F3, ADR-5).
//
// ADR-5 (autonomous session dating) is still OPEN — no RTC hardware is in
// hand. This adapter delivers the guarantee Layer D minimally needs, session
// ORDER, plus best-effort real dates:
//
//   * `settime` (console) -> settimeofday + NVS "was set" marker; dates are
//     real from then on and survive soft resets (the S3's RTC domain keeps
//     system time across resets, not across power loss)
//   * after power loss system time restarts at 1970; begin() detects
//     now < last-checkpoint and jumps time to checkpoint+2s, so session
//     files still sort correctly and dates stay near the last known time
//   * checkpoint() is called by the app at session end (and periodically)
//
// Swapping in a battery RTC later = replacing this adapter; nothing else
// in the firmware changes. That is the port doing its job.
#pragma once

#include <Preferences.h>

#include <sys/time.h>
#include <time.h>

#include "edrum/hal/ports.h"
#include "edrum/timefmt.h"

namespace edrum {
namespace platform {

class Esp32WallClock : public hal::IWallClock {
public:
    void begin(int16_t tz_offset_min) {
        tz_ = tz_offset_min;
        prefs_.begin("edrum", false);
        was_set_ = prefs_.getBool("tset", false);
        const int64_t checkpoint = prefs_.getLong64("tchk", 0);
        const int64_t now = (int64_t)time(nullptr);
        if (now < checkpoint) {
            // power loss reset system time: restore ordering
            struct timeval tv;
            tv.tv_sec = (time_t)(checkpoint + 2);
            tv.tv_usec = 0;
            settimeofday(&tv, nullptr);
        }
    }

    bool now(DateTime& out) override {
        const int64_t epoch = (int64_t)time(nullptr);
        epoch_to_datetime(epoch, tz_, out);
        return true;  // 1970-based when unset — still totally ordered
    }

    // Console `settime <iso>`: the DateTime carries its own tz offset.
    bool set_time(const DateTime& dt) {
        const int64_t epoch = datetime_to_epoch(dt);
        struct timeval tv;
        tv.tv_sec = (time_t)epoch;
        tv.tv_usec = 0;
        if (settimeofday(&tv, nullptr) != 0) return false;
        tz_ = dt.tz_offset_min;
        was_set_ = true;
        prefs_.putBool("tset", true);
        checkpoint();
        return true;
    }

    // Persist a floor for post-power-loss ordering. Cheap; call at session
    // end and on a slow periodic tick.
    void checkpoint() { prefs_.putLong64("tchk", (int64_t)time(nullptr)); }

    bool time_was_set() const { return was_set_; }

private:
    Preferences prefs_;
    int16_t tz_ = 0;
    bool was_set_ = false;
};

}  // namespace platform
}  // namespace edrum
