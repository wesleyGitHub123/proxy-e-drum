// Capture task (core 0) — the single ring producer and the heart of the
// realtime path. One loop iteration:
//   1. usb->poll(): client events + transfer completions (stamp -> parse ->
//      classify -> FSM -> ring, all inside this task's context)
//   2. drain the control queue (console/gesture declarations)
//   3. step the synthetic burst generator (Experiment 3)
//   4. FSM tick (auto-pause / idle-end) + gesture sequence poll
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
        Serial.println(connected ? "[usb] kit streaming" : "[usb] kit disconnected");
    }

    void on_status(const char* msg) override {
        Serial.print('[');
        Serial.print(millis());
        Serial.print("] ");
        Serial.println(msg);
    }
};

CaptureSink sink;

// Experiment 3 synthetic producer: emits through the REAL path (FSM -> ring
// -> LogWriter -> SD) so measured stalls are the production stalls.
struct BurstState {
    uint32_t remaining = 0;
    uint64_t interval_us = 0;
    uint64_t next_us = 0;
    bool note_toggle = false;
} burst;

// Snapshot the running click for a grid/enroll declaration (capture spec §5:
// declarations snapshot the RUNNING click). Returns false if no click.
bool click_snapshot(uint64_t now_us, uint16_t* bpm, uint8_t* subdiv, uint64_t* downbeat_us) {
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

void do_grid_start(uint64_t now_us) {
    uint16_t bpm;
    uint8_t subdiv;
    uint64_t downbeat_us;
    if (!click_snapshot(now_us, &bpm, &subdiv, &downbeat_us)) {
        Serial.println("[grid] refused: no running click to snapshot (start one: click <bpm>)");
        return;
    }
    app.fsm->grid_start(now_us, bpm, subdiv, downbeat_us);
    Serial.printf("[grid] span open @ %u bpm /%u\n", (unsigned)bpm, (unsigned)subdiv);
}

void handle_control(const ControlMsg& msg, uint64_t now_us) {
    switch (msg.op) {
        case ControlMsg::Op::Bookmark:
            app.fsm->bookmark(now_us);
            Serial.println("[mark] bookmark");
            break;
        case ControlMsg::Op::GridStart:
            do_grid_start(now_us);
            break;
        case ControlMsg::Op::GridEnd:
            Serial.println(app.fsm->grid_end(now_us) ? "[grid] span closed"
                                                     : "[grid] no span open");
            break;
        case ControlMsg::Op::GridToggle:
            if (app.fsm->grid_open()) {
                app.fsm->grid_end(now_us);
                Serial.println("[grid] span closed (gesture)");
            } else {
                do_grid_start(now_us);
            }
            break;
        case ControlMsg::Op::EnrollStart: {
            uint16_t bpm;
            uint8_t subdiv;
            uint64_t downbeat_us;
            if (!click_snapshot(now_us, &bpm, &subdiv, &downbeat_us)) {
                Serial.println("[enroll] refused: no running click to snapshot");
                break;
            }
            app.fsm->enroll_start(now_us, msg.ref, bpm, subdiv, downbeat_us);
            Serial.printf("[enroll] span open: %s\n", msg.ref);
            break;
        }
        case ControlMsg::Op::EnrollEnd:
            Serial.println(app.fsm->enroll_end(now_us) ? "[enroll] span closed"
                                                       : "[enroll] no span open");
            break;
        case ControlMsg::Op::EndSession:
            app.fsm->end_session(now_us);
            app.gestures->reset();
            Serial.println("[session] ended");
            break;
        case ControlMsg::Op::Burst:
            burst.remaining = msg.burst_count;
            burst.interval_us = 1000000ULL / msg.burst_hz;
            burst.next_us = now_us;
            Serial.printf("[burst] %lu synthetic events @ %u Hz through the real path\n",
                          (unsigned long)msg.burst_count, (unsigned)msg.burst_hz);
            break;
    }
}

void step_burst(uint64_t now_us) {
    while (burst.remaining > 0 && now_us >= burst.next_us) {
        burst.note_toggle = !burst.note_toggle;
        app.fsm->on_event(burst.next_us, burst.note_toggle ? 38 : 36,
                          (uint8_t)(64 + (burst.remaining % 60)), 9);
        burst.next_us += burst.interval_us;
        if (--burst.remaining == 0) {
            Serial.println("[burst] done — check `stats` for stall/backlog numbers");
        }
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

    ControlMsg msg;
    while (true) {
        app.usb->poll(appcfg::kCapturePollMs);

        while (xQueueReceive(app.control_queue, &msg, 0) == pdTRUE) {
            handle_control(msg, app.clock.now_us());
        }

        const uint64_t now = app.clock.now_us();
        step_burst(now);
        app.fsm->tick(now);

        const GestureDetector::Action act = app.gestures->poll((uint32_t)(now / 1000));
        if (act == GestureDetector::Action::Bookmark) {
            app.fsm->bookmark(now);
            Serial.println("[mark] bookmark (gesture)");
        } else if (act == GestureDetector::Action::GridToggle) {
            ControlMsg toggle{};
            toggle.op = ControlMsg::Op::GridToggle;
            handle_control(toggle, now);
        }
    }
}
