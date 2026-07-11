// Serial console (Communications subsystem) — runs in loop() on core 1.
// Grammar lives in core (console_cmd.*, natively tested); this file only
// reads lines, executes, and prints. Anything that writes to the timeline
// is posted to the capture task via the control queue — the console never
// touches the ring or the FSM directly.
#include "app.h"

#include "edrum/console_cmd.h"
#include "edrum/timefmt.h"

using namespace edrum;

namespace {

char line_buf[128];
size_t line_len = 0;

void post(const ControlMsg& msg) {
    if (xQueueSend(app.control_queue, &msg, 0) != pdTRUE) {
        Serial.println("[console] busy: control queue full, try again");
    }
}

void print_stats() {
    const Counters& c = app.counters;
    Serial.println("---- stats ----------------------------------------------------");
    Serial.printf("usb      transfers=%lu errors=%lu midi=%lu realtime_dropped=%lu\n",
                  (unsigned long)c.usb_transfers, (unsigned long)c.usb_transfer_errors,
                  (unsigned long)c.midi_msgs, (unsigned long)c.realtime_dropped);
    Serial.printf("         cb_gap max=%luus >2ms=%lu >10ms=%lu   (Experiment 1)\n",
                  (unsigned long)c.cb_gap_max_us, (unsigned long)c.cb_gap_over_2ms,
                  (unsigned long)c.cb_gap_over_10ms);
    Serial.printf("capture  events=%lu ctrls=%lu sysex=%lu(trunc %lu) cache_flushes=%lu\n",
                  (unsigned long)c.events, (unsigned long)c.ctrls,
                  (unsigned long)c.sysex_msgs, (unsigned long)c.sysex_truncated,
                  (unsigned long)c.ctrl_cache_flushes);
    Serial.printf("ring     depth=%lu high_water=%lu/%lu DROPS=%lu   (must stay 0)\n",
                  (unsigned long)app.ring.size(), (unsigned long)app.ring.high_water(),
                  (unsigned long)app.ring.capacity(), (unsigned long)c.ring_drops);
    Serial.printf("storage  lines=%lu bytes=%lu syncs=%lu errors=%lu files=%lu\n",
                  (unsigned long)c.storage_lines, (unsigned long)c.storage_bytes,
                  (unsigned long)c.storage_syncs, (unsigned long)c.storage_write_errors,
                  (unsigned long)c.files_opened);
    Serial.printf("         stall max=%luus >50ms=%lu serialize_err=%lu (Experiment 3)\n",
                  (unsigned long)c.write_stall_max_us, (unsigned long)c.write_stall_over_50ms,
                  (unsigned long)c.serialize_errors);
    Serial.printf("session  active=%d paused=%d grid=%d enroll=%d started=%lu ended=%lu\n",
                  (int)app.fsm->in_session(), (int)app.fsm->paused(),
                  (int)app.fsm->grid_open(), (int)app.fsm->enroll_open(),
                  (unsigned long)c.sessions_started, (unsigned long)c.sessions_ended);
    Serial.printf("click    rendered=%lu late_max=%luus   sd=%s audio=%s usb=%s\n",
                  (unsigned long)c.clicks_rendered, (unsigned long)c.click_late_us_max,
                  app.storage_ok ? "ok" : "FAILED", app.audio_ok ? "ok" : "FAILED",
                  (app.usb != nullptr && app.usb->device_streaming()) ? "streaming"
                                                                      : "no device");
    Serial.println("----------------------------------------------------------------");
}

void execute(const Command& cmd) {
    switch (cmd.kind) {
        case Command::Kind::None:
            break;
        case Command::Kind::Invalid:
            Serial.printf("[console] %s\n", cmd.error);
            break;
        case Command::Kind::Help:
            Serial.println(console_help());
            break;
        case Command::Kind::Stats:
            print_stats();
            break;
        case Command::Kind::Time: {
            DateTime dt;
            app.wall.now(dt);
            char iso[32];
            format_iso(dt, iso, sizeof(iso));
            Serial.printf("[time] %s%s\n", iso,
                          app.wall.time_was_set() ? "" : "  (UNSET — ordering only; use settime)");
            break;
        }
        case Command::Kind::SetTime:
            if (app.wall.set_time(cmd.dt)) {
                Serial.println("[time] set");
            } else {
                Serial.println("[time] set FAILED");
            }
            break;
        case Command::Kind::ClickStart: {
            // First beat ~200 ms out so the downbeat is never already late.
            const uint64_t start = app.clock.now_us() + 200000;
            taskENTER_CRITICAL(&app.click_mux);
            app.click_sched.start(start, cmd.bpm, cmd.subdiv);
            taskEXIT_CRITICAL(&app.click_mux);
            Serial.printf("[click] %u bpm /%u\n", (unsigned)cmd.bpm, (unsigned)cmd.subdiv);
            if (app.fsm->grid_open()) {
                // §5 param-change rule: re-declare the span at the new tempo
                ControlMsg msg{};
                msg.op = ControlMsg::Op::GridStart;
                post(msg);
            }
            break;
        }
        case Command::Kind::ClickStop:
            taskENTER_CRITICAL(&app.click_mux);
            app.click_sched.stop();
            taskEXIT_CRITICAL(&app.click_mux);
            Serial.println("[click] off");
            break;
        case Command::Kind::GridStart: {
            ControlMsg msg{};
            msg.op = ControlMsg::Op::GridStart;
            post(msg);
            break;
        }
        case Command::Kind::GridEnd: {
            ControlMsg msg{};
            msg.op = ControlMsg::Op::GridEnd;
            post(msg);
            break;
        }
        case Command::Kind::Bookmark: {
            ControlMsg msg{};
            msg.op = ControlMsg::Op::Bookmark;
            post(msg);
            break;
        }
        case Command::Kind::EnrollStart: {
            ControlMsg msg{};
            msg.op = ControlMsg::Op::EnrollStart;
            strncpy(msg.ref, cmd.ref, sizeof(msg.ref) - 1);
            post(msg);
            break;
        }
        case Command::Kind::EnrollEnd: {
            ControlMsg msg{};
            msg.op = ControlMsg::Op::EnrollEnd;
            post(msg);
            break;
        }
        case Command::Kind::EndSession: {
            ControlMsg msg{};
            msg.op = ControlMsg::Op::EndSession;
            post(msg);
            break;
        }
        case Command::Kind::Burst: {
            ControlMsg msg{};
            msg.op = ControlMsg::Op::Burst;
            msg.burst_count = cmd.burst_count;
            msg.burst_hz = cmd.burst_hz;
            post(msg);
            break;
        }
    }
}

}  // namespace

void console_poll() {
    while (Serial.available() > 0) {
        const char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (line_len > 0) {
                line_buf[line_len] = '\0';
                line_len = 0;
                execute(parse_command(line_buf));
            }
        } else if (line_len < sizeof(line_buf) - 1) {
            line_buf[line_len++] = c;
        }
    }
}
