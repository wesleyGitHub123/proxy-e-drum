// Capture task (core 0) — the single ring producer and the heart of the
// realtime path. One loop iteration:
//   1. usb->poll(): client events + transfer completions (stamp -> parse ->
//      classify -> FSM -> ring, all inside this task's context)
//   2. drain the control queue (console/gesture/future-controller
//      declarations) through the single ControlDispatcher
//   3. drain the diag queue (Experiment 3 synthetic burst)
//   4. step the synthetic burst generator
//   5. FSM tick (auto-pause / idle-end) + gesture sequence poll, itself
//      routed through the same ControlDispatcher as every other controller
#include "app.h"

#include "edrum/classify.h"

using namespace edrum;

namespace {

// USB sink: classify at the arrival edge and hand to the session FSM.
struct CaptureSink : platform::UsbHostMidi::Sink {
    void on_midi(uint64_t t_us, const MidiMsg& msg) override {
        switch (classify(msg)) {
            case MsgClass::Event:
                app.fsm->on_event(t_us, msg.data1, msg.data2, msg.channel());
                app.gestures->on_event((uint32_t)(t_us / 1000), msg.data1, msg.data2);
                break;
            case MsgClass::Ctrl:
                app.fsm->on_ctrl(t_us, msg);
                break;
            case MsgClass::DropRealtime:
                app.counters.realtime_dropped++;
                break;
        }
    }

    void on_sysex(uint64_t t_us, const uint8_t* data, uint8_t len, bool truncated) override {
        app.fsm->on_sysex(t_us, data, len, truncated);
    }

    void on_device(bool connected) override {
        // Session continues across an unplug; the idle failsafe ends it.
        if (app.sync_mode) return;  // never interleave text into a frame
        Serial.println(connected ? "[usb] kit streaming" : "[usb] kit disconnected");
    }

    void on_status(const char* msg) override {
        if (app.sync_mode) return;  // never interleave text into a frame
        Serial.print('[');
        Serial.print(millis());
        Serial.print("] ");
        Serial.println(msg);
    }
};

CaptureSink sink;

// Adapts the spinlock-shared click scheduler to hal::IClickSnapshot so
// ControlDispatcher (edrum_core, portable) never touches app.click_sched or
// the FreeRTOS spinlock directly.
struct ClickSnapshotAdapter : hal::IClickSnapshot {
    bool snapshot(uint64_t now_us, uint16_t* bpm, uint8_t* subdiv,
                 uint64_t* downbeat_us) override {
        bool ok;
        taskENTER_CRITICAL(&app.click_mux);
        ok = app.click_sched.running();
        if (ok) {
            *bpm = app.click_sched.bpm();
            *subdiv = app.click_sched.subdiv();
            *downbeat_us = app.click_sched.next_edge_at_or_after(now_us);
        }
        taskEXIT_CRITICAL(&app.click_mux);
        return ok;
    }
};

ClickSnapshotAdapter click_snapshot_adapter;

// Experiment 3 synthetic producer: emits through the REAL path (FSM -> ring
// -> LogWriter -> SD) so measured stalls are the production stalls.
struct BurstState {
    uint32_t remaining = 0;
    uint64_t interval_us = 0;
    uint64_t next_us = 0;
    bool note_toggle = false;
} burst;

// The dispatcher does no I/O itself (engine-purity discipline); this is
// where its ControlResult becomes the operator-facing console message.
// During framed sync mode the declaration still applied — only the print is
// suppressed (text inside a frame would corrupt it, decision 1).
void report_control(const ControlResult& r) {
    if (app.sync_mode) return;
    switch (r.outcome) {
        case ControlOutcome::BookmarkAdded:
            Serial.println("[mark] bookmark");
            break;
        case ControlOutcome::GridOpened:
            Serial.printf("[grid] span open @ %u bpm /%u\n", (unsigned)r.bpm,
                          (unsigned)r.subdiv);
            break;
        case ControlOutcome::GridRefusedNoClick:
            Serial.println("[grid] refused: no running click to snapshot (start one: click <bpm>)");
            break;
        case ControlOutcome::GridClosed:
            Serial.println("[grid] span closed");
            break;
        case ControlOutcome::GridNoSpanOpen:
            Serial.println("[grid] no span open");
            break;
        case ControlOutcome::EnrollOpened:
            if (r.ref[0] != '\0') {
                Serial.printf("[enroll] span open: %s\n", r.ref);
            } else {
                Serial.println("[enroll] span open (anonymous — name it later in the brain/UI)");
            }
            break;
        case ControlOutcome::EnrollRefusedNoClick:
            Serial.println("[enroll] refused: no running click to snapshot");
            break;
        case ControlOutcome::EnrollClosed:
            Serial.println("[enroll] span closed");
            break;
        case ControlOutcome::EnrollNoSpanOpen:
            Serial.println("[enroll] no span open");
            break;
        case ControlOutcome::SessionEnded:
            Serial.println("[session] ended");
            break;
    }
}

}  // namespace

void capture_task(void*) {
    static platform::UsbHostMidi usb(app.clock, sink, app.counters);
    app.usb = &usb;
    if (!app.usb->begin(appcfg::kUsbDaemonCore, appcfg::kUsbDaemonPrio)) {
        Serial.println("[usb] FATAL: host stack failed; capture task halted");
        vTaskDelete(nullptr);
        return;
    }

    static ControlDispatcher dispatcher(*app.fsm, click_snapshot_adapter, *app.gestures);

    ControlMsg msg;
    DiagMsg dmsg;
    while (true) {
        app.usb->poll(appcfg::kCapturePollMs);

        while (xQueueReceive(app.control_queue, &msg, 0) == pdTRUE) {
            report_control(dispatcher.dispatch(msg, app.clock.now_us()));
        }
        while (xQueueReceive(app.diag_queue, &dmsg, 0) == pdTRUE) {
            burst.remaining = dmsg.burst_count;
            burst.interval_us = 1000000ULL / dmsg.burst_hz;
            burst.next_us = app.clock.now_us();
            if (!app.sync_mode) {
                Serial.printf("[burst] %lu synthetic events @ %u Hz through the real path\n",
                              (unsigned long)dmsg.burst_count, (unsigned)dmsg.burst_hz);
            }
        }

        const uint64_t now = app.clock.now_us();
        while (burst.remaining > 0 && now >= burst.next_us) {
            burst.note_toggle = !burst.note_toggle;
            app.fsm->on_event(burst.next_us, burst.note_toggle ? 38 : 36,
                              (uint8_t)(64 + (burst.remaining % 60)), 9);
            burst.next_us += burst.interval_us;
            if (--burst.remaining == 0 && !app.sync_mode) {
                Serial.println("[burst] done — check `stats` for stall/backlog numbers");
            }
        }
        app.fsm->tick(now);

        const GestureDetector::Action act = app.gestures->poll((uint32_t)(now / 1000));
        ControlMsg gmsg{};
        bool has_gesture = true;
        switch (act) {
            case GestureDetector::Action::Bookmark:
                gmsg.op = ControlMsg::Op::Bookmark;
                break;
            case GestureDetector::Action::GridToggle:
                gmsg.op = ControlMsg::Op::GridToggle;
                break;
            case GestureDetector::Action::EnrollToggle:
                gmsg.op = ControlMsg::Op::EnrollToggle;
                break;
            case GestureDetector::Action::None:
            default:
                has_gesture = false;
                break;
        }
        if (has_gesture) {
            report_control(dispatcher.dispatch(gmsg, now));
        }
    }
}
