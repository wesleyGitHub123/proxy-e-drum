// Health/experiment counters — the probe's instrumentation instinct made
// permanent (firmware architecture plan §1). These are the metrics the
// roadmap's Experiment 1 (USB timing) and Experiment 3 (storage stall
// budget) are expressed in.
//
// Concurrency note: each field has exactly one writer task; cross-task reads
// (console `stats`) tolerate word-tearing-free racy reads of 32-bit aligned
// values on both xtensa and the native host. No locks on the hot path.
#pragma once

#include <stdint.h>

namespace edrum {

struct Counters {
    // capture edge (writer: capture task)
    uint32_t usb_transfers = 0;
    uint32_t usb_transfer_errors = 0;
    uint32_t midi_msgs = 0;
    uint32_t events = 0;
    uint32_t ctrls = 0;
    uint32_t realtime_dropped = 0;
    uint32_t sysex_msgs = 0;
    uint32_t sysex_truncated = 0;
    uint32_t ring_drops = 0;          // push failed: ring full — must stay 0
    uint32_t serialize_errors = 0;    // record_line returned 0 — must stay 0
    uint32_t sessions_started = 0;
    uint32_t sessions_ended = 0;
    uint32_t ctrl_cache_flushes = 0;
    uint32_t ctrl_nosession_dropped = 0;  // non-cacheable ctrl with no session

    // Experiment 1: spacing between consecutive transfer callbacks while a
    // device is streaming (µs). A tail far beyond the ~1 ms USB frame floor
    // means the host path adds its own scheduling layer (capture spec §3).
    uint32_t cb_gap_max_us = 0;
    uint32_t cb_gap_over_2ms = 0;
    uint32_t cb_gap_over_10ms = 0;

    // storage (writer: storage task)
    uint32_t storage_lines = 0;
    uint32_t storage_bytes = 0;
    uint32_t storage_write_errors = 0;
    uint32_t storage_syncs = 0;
    uint32_t write_stall_max_us = 0;   // Experiment 3 headline
    uint32_t write_stall_over_50ms = 0;
    uint32_t files_opened = 0;

    // click (writer: click task)
    uint32_t clicks_rendered = 0;
    uint32_t click_late_us_max = 0;    // scheduled-vs-rendered lateness
};

}  // namespace edrum
