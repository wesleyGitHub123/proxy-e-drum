// Native USB host MIDI link — the F1 subsystem (roadmap Experiment 1,
// ADR-1). Evolves the validated enumeration probe (tools/probe) into the
// production data path:
//
//   attach -> open -> read config descriptor -> core::find_midi_streaming()
//   -> claim the MIDI Streaming interface -> submit IN transfers
//   -> completion callback: STAMP FIRST (capture spec §3 Rule 2), then parse
//      (core UsbMidiParser) and forward (t_us, msg) to the sink
//
// Task model (per the ESP-IDF host library):
//   * usb_lib daemon task — created by begin(); drives hub/enumeration
//   * client — driven by poll(), which the CAPTURE TASK calls in its loop;
//     transfer callbacks therefore run in the capture task's context, which
//     preserves the single-producer discipline of the ring (ring.h)
//
// Deferred open/close out of the callback (close processed first for fast
// unplug/replug) is carried over from the probe unchanged — that pattern is
// the part of the probe that was always production code.
#pragma once

#include <stdint.h>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
}

#include "edrum/counters.h"
#include "edrum/descwalk.h"
#include "edrum/hal/ports.h"
#include "edrum/midimsg.h"
#include "edrum/usbmidi.h"

namespace edrum {
namespace platform {

class UsbHostMidi : private UsbMidiParser::Handler {
public:
    // All callbacks run in the capture task (inside poll()).
    struct Sink {
        virtual void on_midi(uint64_t t_us, const MidiMsg& msg) = 0;
        virtual void on_sysex(uint64_t t_us, const uint8_t* data, uint8_t len,
                              bool truncated) = 0;
        virtual void on_device(bool connected) = 0;
        virtual void on_status(const char* msg) = 0;  // human-readable stage lines
        virtual ~Sink() = default;
    };

    UsbHostMidi(hal::IClock& clock, Sink& sink, Counters& counters)
        : clock_(clock), sink_(sink), counters_(counters), parser_(*this) {}

    // Install the host stack and spawn the usb_lib daemon task (pinned to
    // `daemon_core`). Returns false if the stack refuses to install.
    bool begin(int daemon_core, unsigned daemon_priority);

    // One client-loop iteration: handle client events (<= timeout_ms), then
    // any deferred open/close work. Call from the capture task, forever.
    void poll(uint32_t timeout_ms);

    bool device_streaming() const { return streaming_; }

private:
    static constexpr int kNumTransfers = 4;  // in-flight IN transfers

    static void client_event_cb(const usb_host_client_event_msg_t* msg, void* arg);
    static void transfer_cb(usb_transfer_t* transfer);
    static void usb_lib_task(void* arg);

    void handle_new_device(uint8_t address);
    void handle_device_gone();
    void open_and_claim();
    void teardown_device();
    void on_transfer_done(usb_transfer_t* transfer);

    // UsbMidiParser::Handler — forwards with the transfer's arrival stamp.
    void on_midi(const MidiMsg& msg) override;
    void on_sysex(const uint8_t* data, uint8_t len, bool truncated) override;

    hal::IClock& clock_;
    Sink& sink_;
    Counters& counters_;
    UsbMidiParser parser_;

    usb_host_client_handle_t client_ = nullptr;
    usb_device_handle_t device_ = nullptr;
    usb_transfer_t* transfers_[kNumTransfers] = {nullptr};
    MidiStreamingInfo iface_{};

    volatile bool host_installed_ = false;
    bool open_pending_ = false;
    bool close_pending_ = false;
    uint8_t pending_address_ = 0;
    bool claimed_ = false;
    bool streaming_ = false;

    uint64_t current_t_us_ = 0;  // stamp of the transfer being parsed
};

}  // namespace platform
}  // namespace edrum
