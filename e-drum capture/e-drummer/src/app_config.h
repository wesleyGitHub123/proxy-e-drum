// Build-time application configuration: pin map, task placement, tuning.
//
// ── PIN MAP ────────────────────────────────────────────────────────────────
// ⚠ VERIFY at wiring time (capture spec §8.4). These are deliberate defaults,
// not architecture: change them here and reflash. Chosen to avoid:
//   * GPIO19/20      native USB D-/D+ (the kit's port!)
//   * GPIO26-32      SPI flash
//   * GPIO33-37      octal PSRAM on N16R8 modules
//   * GPIO0/3/45/46  strapping pins
//   * GPIO43/44      UART0 (the debug/flash port, spec §8.3)
#pragma once

#include <stdint.h>

namespace appcfg {

// SPI microSD module (FSPI defaults on the S3)
constexpr int kSdCs = 10;
constexpr int kSdMosi = 11;
constexpr int kSdSck = 12;
constexpr int kSdMiso = 13;
constexpr uint32_t kSdMaxHz = 16000000;

// I2S DAC (PCM5102 / MAX98357A -> TD-02 MIX IN, capture spec §4)
constexpr int kI2sBclk = 14;
constexpr int kI2sLrck = 15;
constexpr int kI2sDout = 16;
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kClickBlockFrames = 48;  // 1 ms blocks

// Storage paths
constexpr const char* kSessionDir = "/sessions";
constexpr const char* kConfigPath = "/config.txt";

// Ring / queues (Experiment 3 will size these against measured stalls;
// 512 x 64 B = 32 KB internal SRAM, >1 s of worst-case drumming)
constexpr uint32_t kRingCapacity = 512;  // power of two
constexpr uint32_t kControlQueueLen = 16;

// Storage sync policy (LogWriter; Experiment 3 knobs)
constexpr uint32_t kSyncEveryBytes = 4096;
constexpr uint32_t kSyncEveryMs = 250;

// Task placement (capture spec §2: capture+click vs storage on separate
// cores; Arduino's loopTask — our console — also lives on core 1)
constexpr int kUsbDaemonCore = 0;
constexpr unsigned kUsbDaemonPrio = 5;
constexpr int kCaptureCore = 0;
constexpr unsigned kCapturePrio = 10;
constexpr int kClickCore = 0;
constexpr unsigned kClickPrio = 11;  // tiny work, tightest deadline
constexpr int kStorageCore = 1;
constexpr unsigned kStoragePrio = 5;

constexpr uint32_t kCapturePollMs = 5;
constexpr uint32_t kStoragePollMs = 10;

}  // namespace appcfg
