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
#include "edrum/control_dispatch.h"
#include "edrum/control_msg.h"
#include "edrum/counters.h"
#include "edrum/frame.h"
#include "edrum/gesture.h"
#include "edrum/logwriter.h"
#include "edrum/records.h"
#include "edrum/ring.h"
#include "edrum/session_fsm.h"
#include "edrum/sync_service.h"
#include "edrum/usbmidi.h"
#include "edrum/platform/esp32_clock.h"
#include "edrum/platform/esp32_random.h"
#include "edrum/platform/esp32_wallclock.h"
#include "edrum/platform/i2s_click_out.h"
#include "edrum/platform/sd_storage.h"
#include "edrum/platform/serial_link.h"
#include "edrum/platform/usb_host_midi.h"

#include "app_config.h"

// ControlMsg (edrum/control_msg.h) is the portable, source-agnostic
// session-control vocabulary: console, gestures, and any future controller
// (desktop/mobile app, hardware button, BLE, network API) all construct one
// and hand it to the single ControlDispatcher, which is the only thing that
// calls into the session FSM. Declarations must still be applied by the
// capture task (the single ring producer) to preserve the total timeline
// order the append-only log requires — that's a property of the queue
// below, not of ControlMsg itself.

// Diagnostic/experiment-harness message: the synthetic burst load generator
// (Experiment 3). Deliberately NOT part of ControlMsg — it's a firmware
// self-test concern, not a session declaration any real controller should
// need to know about.
struct DiagMsg {
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
    QueueHandle_t control_queue = nullptr;  // ControlMsg — session declarations
    QueueHandle_t diag_queue = nullptr;     // DiagMsg — experiment harness only

    // device<->brain sync link (capture spec §13). Modal arbitration
    // (decision 1): the console `sync` command sets sync_mode and stops
    // reading the line; the storage task pumps the framed protocol and
    // clears the flag on BYE or idle timeout. While set, tasks suppress
    // console prints so text never interleaves into a frame.
    edrum::platform::SerialLink* link = nullptr;
    edrum::SyncService* sync = nullptr;
    volatile bool sync_mode = false;
};

extern App app;

void capture_task(void* arg);
void storage_task(void* arg);
void click_task(void* arg);
void console_poll();  // runs in loop() on core 1
