// Shared application context — the objects the composition root constructs
// once and the tasks share. This is deliberately the ONLY place subsystems
// meet: core modules never include this; tasks never construct anything.
//
// Producer/consumer discipline (architecture plan §3/§8):
//   * capture task = the ONLY ring producer (USB callbacks run inside its
//     poll(); console/gestures reach it via the control queue)
//   * storage task = the ONLY ring consumer
//   * click task touches only the click scheduler/renderer (spinlock-shared
//     with the capture/console paths for start/stop/snapshot)
#pragma once

#include <Arduino.h>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
}

#include "edrum/click_render.h"
#include "edrum/click_sched.h"
#include "edrum/config.h"
#include "edrum/counters.h"
#include "edrum/gesture.h"
#include "edrum/logwriter.h"
#include "edrum/records.h"
#include "edrum/ring.h"
#include "edrum/session_fsm.h"
#include "edrum/usbmidi.h"
#include "edrum/platform/esp32_clock.h"
#include "edrum/platform/esp32_random.h"
#include "edrum/platform/esp32_wallclock.h"
#include "edrum/platform/i2s_click_out.h"
#include "edrum/platform/sd_storage.h"
#include "edrum/platform/usb_host_midi.h"

#include "app_config.h"

// Control-plane message: console (loop, core 1) / gestures -> capture task.
// Declarations must be executed by the single ring producer to preserve the
// total timeline order the append-only log requires.
struct ControlMsg {
    enum class Op : uint8_t {
        Bookmark,
        GridStart,   // snapshot the running click
        GridEnd,
        GridToggle,  // gesture: start if closed, end if open
        EnrollStart,
        EnrollEnd,
        EndSession,
        Burst,       // Experiment 3 synthetic load
    };
    Op op;
    char ref[edrum::kProfileRefMax + 1];
    uint32_t burst_count;
    uint16_t burst_hz;
};

struct App {
    // config (loaded from SD at boot; defaults otherwise)
    edrum::Config cfg;
    edrum::MetaStatic meta{};

    // ports / adapters
    edrum::platform::Esp32Clock clock;
    edrum::platform::Esp32Random random;
    edrum::platform::Esp32WallClock wall;
    edrum::platform::SdStorage storage;
    bool storage_ok = false;

    // capture core
    edrum::Counters counters;
    edrum::SpscRing<edrum::CaptureRecord, appcfg::kRingCapacity> ring;
    edrum::SessionController* fsm = nullptr;
    edrum::GestureDetector* gestures = nullptr;
    edrum::platform::UsbHostMidi* usb = nullptr;

    // storage
    edrum::LogWriter* writer = nullptr;

    // click (spinlock-shared: console starts/stops, capture snapshots,
    // click task renders)
    edrum::ClickScheduler click_sched;
    edrum::ClickRenderer* click_render = nullptr;
    edrum::platform::I2sClickOut* audio = nullptr;
    bool audio_ok = false;
    portMUX_TYPE click_mux = portMUX_INITIALIZER_UNLOCKED;

    // control plane
    QueueHandle_t control_queue = nullptr;
};

extern App app;

void capture_task(void* arg);
void storage_task(void* arg);
void click_task(void* arg);
void console_poll();  // runs in loop() on core 1
