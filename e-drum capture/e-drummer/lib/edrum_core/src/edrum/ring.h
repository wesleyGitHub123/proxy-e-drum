// Fixed-capacity single-producer / single-consumer ring buffer — the
// architectural seam of the firmware (capture spec §2): the capture path
// pushes, the storage task pops, and neither blocks the other. Lock-free via
// acquire/release atomics; capacity is a power of two; no allocation ever.
//
// Producer discipline: exactly ONE task calls push(); exactly ONE task calls
// pop(). All record producers (USB capture, declarations, session FSM) are
// funneled through the capture task to preserve both SPSC safety and the
// total timeline order the append-only log requires (capture spec §6).
#pragma once

#include <stdint.h>

#include <atomic>

namespace edrum {

template <typename T, uint32_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");

public:
    // Producer side. Returns false (and writes nothing) when full.
    bool push(const T& v) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= N) {
            return false;
        }
        buf_[head & (N - 1)] = v;
        head_.store(head + 1, std::memory_order_release);
        const uint32_t depth = head + 1 - tail;
        if (depth > high_water_) high_water_ = depth;  // producer-only write
        return true;
    }

    // Consumer side. Returns false when empty.
    bool pop(T& out) {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = buf_[tail & (N - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    uint32_t size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

    bool empty() const { return size() == 0; }
    static constexpr uint32_t capacity() { return N; }

    // Deepest backlog ever observed (Experiment 3's headline metric).
    // Producer-written; racy-read from other tasks is fine for diagnostics.
    uint32_t high_water() const { return high_water_; }

private:
    T buf_[N];
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
    uint32_t high_water_ = 0;
};

}  // namespace edrum
