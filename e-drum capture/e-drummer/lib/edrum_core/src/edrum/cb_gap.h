// Experiment 1 instrumentation, made honest — inter-arrival spacing between
// consecutive *completed* USB IN transfers.
//
// The trap this exists to avoid: reading the raw MAX gap as "latency". An idle
// USB-MIDI IN endpoint NAKs, so no completion callback fires until the kit
// actually sends a packet. A 1.8 s max gap is therefore just the drummer
// resting, not the host stalling — the max says nothing about host health.
//
// The signal for "does the native host add a second scheduling layer?"
// (roadmap Experiment 1) lives in the LOWER tail: when two MIDI messages arrive
// close together, the gap should sit near the ~1 ms full-speed frame floor, not
// at some inflated host-scheduling quantum. So we separate BURST spacing (gaps
// <= kBurstThresholdUs — data actually flowing) from idle rests, keep the
// minimum gap, and bin the whole distribution into a coarse histogram so its
// shape is visible instead of collapsed to a max.
//
// Purity: no Arduino/ESP-IDF includes, so the native suite exercises the whole
// classifier without hardware. The USB host adapter only feeds it arrival
// stamps. One writer (the capture task); the 32-bit result fields tolerate the
// console's racy cross-task reads (see counters.h). The 64-bit hot-path state
// (last_us) is never read cross-task.
#pragma once

#include <stdint.h>

namespace edrum {

struct CbGapStats {
    // A gap at or under this counts as "within a burst": ~15 ms is the far edge
    // of a fast roll's inter-event spacing, so anything wider is a rest (or the
    // gaps between musical events), not the host path.
    static constexpr uint32_t kBurstThresholdUs = 15000;

    // Coarse distribution: exclusive upper edges in µs; the final bucket is the
    // overflow (>= the last edge). Labels track these in console.cpp.
    static constexpr int kBuckets = 8;

    // --- hot-path state (capture task only; never read cross-task) ---
    uint64_t last_us = 0;
    bool have_last = false;

    // --- whole-stream headline (names mirror the original counters) ---
    uint32_t samples = 0;    // gaps measured (transfers minus stream-starts)
    uint32_t max_us = 0;
    uint32_t over_2ms = 0;
    uint32_t over_10ms = 0;

    // --- lower-tail discriminator: the part that actually answers Experiment 1 ---
    uint32_t min_us = 0;         // smallest gap seen; valid iff samples > 0
    uint32_t burst_samples = 0;  // gaps <= kBurstThresholdUs (data flowing)
    uint32_t burst_max_us = 0;   // largest gap *inside* a burst — the tail that matters

    uint32_t hist[kBuckets] = {0};

    // Exclusive upper edge of bucket i (i == kBuckets-1 is the overflow bucket).
    static uint32_t bucket_edge_us(int i) {
        static const uint32_t edges[kBuckets] = {
            1000, 2000, 5000, 10000, 50000, 200000, 1000000, 0xFFFFFFFFu};
        return edges[i];
    }

    // Feed one completed-transfer arrival stamp. Returns the gap in µs since the
    // previous transfer (0 on the first call after begin_stream()).
    uint32_t record(uint64_t t_us) {
        if (!have_last) {
            have_last = true;
            last_us = t_us;
            return 0;
        }
        uint64_t gap64 = t_us - last_us;
        last_us = t_us;
        uint32_t gap = gap64 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)gap64;

        samples++;
        if (gap > max_us) max_us = gap;
        if (samples == 1 || gap < min_us) min_us = gap;
        if (gap > 2000) over_2ms++;
        if (gap > 10000) over_10ms++;

        if (gap <= kBurstThresholdUs) {
            burst_samples++;
            if (gap > burst_max_us) burst_max_us = gap;
        }

        int b = kBuckets - 1;
        for (int i = 0; i < kBuckets - 1; ++i) {
            if (gap < bucket_edge_us(i)) {
                b = i;
                break;
            }
        }
        hist[b]++;
        return gap;
    }

    // Call on (re)connect: the next record() starts a fresh gap so nothing is
    // measured across a disconnect. Accumulated metrics are preserved.
    void begin_stream() { have_last = false; }
};

}  // namespace edrum
