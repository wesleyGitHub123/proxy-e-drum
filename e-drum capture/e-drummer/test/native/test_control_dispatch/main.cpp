// ControlDispatcher: the single ControlMsg -> SessionController translation
// path every controller (console, gesture, and future desktop/mobile/BLE/
// network controllers) shares. This is the logic that used to live only in
// capture_task.cpp's handle_control(), reachable exclusively on hardware —
// it now gets native coverage, including the click-snapshot-refusal path
// and the labeled-vs-anonymous enrollment distinction the architecture
// review turned on.
#include <string.h>

#include <unity.h>

#include "edrum/control_dispatch.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

namespace {

struct FakeWall : hal::IWallClock {
    bool now(DateTime& out) override {
        out.year = 2026;
        out.month = 7;
        out.day = 11;
        out.hour = 12;
        out.minute = 0;
        out.second = 0;
        out.tz_offset_min = 480;
        return true;
    }
};

struct FakeRandom : hal::IRandom {
    void fill(uint8_t* buf, size_t len) override {
        for (size_t i = 0; i < len; ++i) buf[i] = (uint8_t)(0x11 * ((i % 8) + 1));
    }
};

struct VecSink : RecordSink {
    CaptureRecord recs[256];
    int n = 0;
    bool push(const CaptureRecord& r) override {
        if (n >= 256) return false;
        recs[n++] = r;
        return true;
    }
    const CaptureRecord& operator[](int i) const { return recs[i]; }
};

// No click running until a test opts in — exercises the "declarations
// snapshot the RUNNING click" refusal path (capture spec §5) without any
// FreeRTOS spinlock or hardware.
struct FakeClick : hal::IClickSnapshot {
    bool running = false;
    uint16_t bpm = 120;
    uint8_t subdiv = 4;
    uint64_t downbeat_us = 0;
    bool snapshot(uint64_t, uint16_t* out_bpm, uint8_t* out_subdiv,
                 uint64_t* out_downbeat_us) override {
        if (!running) return false;
        *out_bpm = bpm;
        *out_subdiv = subdiv;
        *out_downbeat_us = downbeat_us;
        return true;
    }
};

struct Rig {
    FakeWall wall;
    FakeRandom rnd;
    VecSink sink;
    Counters c;
    SessionController::Cfg fcfg;
    SessionController fsm;
    GestureDetector::Cfg gcfg;
    GestureDetector gestures;
    FakeClick click;
    ControlDispatcher dispatcher;

    Rig()
        : fsm(fcfg, sink, wall, rnd, c),
          gestures(gcfg),
          dispatcher(fsm, click, gestures) {}
};

constexpr uint64_t MS = 1000;  // µs per ms

ControlMsg op(ControlMsg::Op o) {
    ControlMsg m{};
    m.op = o;
    return m;
}

}  // namespace

static void test_bookmark_dispatches_to_fsm(void) {
    Rig r;
    const ControlResult res = r.dispatcher.dispatch(op(ControlMsg::Op::Bookmark), 1000 * MS);
    TEST_ASSERT_TRUE(res.outcome == ControlOutcome::BookmarkAdded);
    TEST_ASSERT_TRUE(r.fsm.in_session());
    TEST_ASSERT_TRUE(r.sink[r.sink.n - 1].type == RecType::Bookmark);
}

static void test_grid_start_refused_without_click(void) {
    Rig r;
    const ControlResult res = r.dispatcher.dispatch(op(ControlMsg::Op::GridStart), 1000 * MS);
    TEST_ASSERT_TRUE(res.outcome == ControlOutcome::GridRefusedNoClick);
    TEST_ASSERT_FALSE(r.fsm.grid_open());
}

static void test_grid_start_snapshots_click(void) {
    Rig r;
    r.click.running = true;
    r.click.bpm = 100;
    r.click.subdiv = 8;
    r.click.downbeat_us = 500 * MS;
    const ControlResult res = r.dispatcher.dispatch(op(ControlMsg::Op::GridStart), 1000 * MS);
    TEST_ASSERT_TRUE(res.outcome == ControlOutcome::GridOpened);
    TEST_ASSERT_EQUAL_UINT16(100, res.bpm);
    TEST_ASSERT_EQUAL_UINT8(8, res.subdiv);
    TEST_ASSERT_TRUE(r.fsm.grid_open());
}

static void test_grid_toggle_opens_then_closes(void) {
    Rig r;
    r.click.running = true;
    ControlResult res = r.dispatcher.dispatch(op(ControlMsg::Op::GridToggle), 1000 * MS);
    TEST_ASSERT_TRUE(res.outcome == ControlOutcome::GridOpened);
    TEST_ASSERT_TRUE(r.fsm.grid_open());

    res = r.dispatcher.dispatch(op(ControlMsg::Op::GridToggle), 2000 * MS);
    TEST_ASSERT_TRUE(res.outcome == ControlOutcome::GridClosed);
    TEST_ASSERT_FALSE(r.fsm.grid_open());
}

static void test_enroll_start_labeled_is_provenance(void) {
    Rig r;
    r.click.running = true;
    ControlMsg msg = op(ControlMsg::Op::EnrollStart);
    strcpy(msg.ref, "shuffle");
    const ControlResult res = r.dispatcher.dispatch(msg, 1000 * MS);
    TEST_ASSERT_TRUE(res.outcome == ControlOutcome::EnrollOpened);
    TEST_ASSERT_EQUAL_STRING("shuffle", res.ref);
    TEST_ASSERT_TRUE(r.fsm.enroll_open());
    TEST_ASSERT_EQUAL_STRING("shuffle", r.sink[r.sink.n - 1].u.enroll.profile_ref);
}

static void test_enroll_start_no_label_is_anonymous(void) {
    Rig r;
    r.click.running = true;
    // ref left empty: zero-friction path (gesture/hardware button/quick
    // app action) — the log must honestly record "no provenance", never
    // invent a name.
    const ControlResult res = r.dispatcher.dispatch(op(ControlMsg::Op::EnrollStart), 1000 * MS);
    TEST_ASSERT_TRUE(res.outcome == ControlOutcome::EnrollOpened);
    TEST_ASSERT_EQUAL_STRING("", res.ref);
    TEST_ASSERT_TRUE(r.fsm.enroll_open());
    TEST_ASSERT_EQUAL_STRING("", r.sink[r.sink.n - 1].u.enroll.profile_ref);
}

static void test_enroll_start_refused_without_click(void) {
    Rig r;
    ControlMsg msg = op(ControlMsg::Op::EnrollStart);
    strcpy(msg.ref, "shuffle");
    const ControlResult res = r.dispatcher.dispatch(msg, 1000 * MS);
    TEST_ASSERT_TRUE(res.outcome == ControlOutcome::EnrollRefusedNoClick);
    TEST_ASSERT_FALSE(r.fsm.enroll_open());
}

static void test_enroll_end_no_span_open(void) {
    Rig r;
    const ControlResult res = r.dispatcher.dispatch(op(ControlMsg::Op::EnrollEnd), 1000 * MS);
    TEST_ASSERT_TRUE(res.outcome == ControlOutcome::EnrollNoSpanOpen);
}

static void test_enroll_toggle_always_anonymous_regardless_of_ref(void) {
    // A toggle-capable controller (gesture, hardware button) has no label
    // slot to offer by construction; even a stray `ref` on the message must
    // not leak through — a controller that has a real label uses
    // EnrollStart directly instead of the toggle.
    Rig r;
    r.click.running = true;
    ControlMsg msg = op(ControlMsg::Op::EnrollToggle);
    strcpy(msg.ref, "should-be-ignored");

    ControlResult res = r.dispatcher.dispatch(msg, 1000 * MS);
    TEST_ASSERT_TRUE(res.outcome == ControlOutcome::EnrollOpened);
    TEST_ASSERT_EQUAL_STRING("", res.ref);
    TEST_ASSERT_TRUE(r.fsm.enroll_open());

    res = r.dispatcher.dispatch(msg, 2000 * MS);
    TEST_ASSERT_TRUE(res.outcome == ControlOutcome::EnrollClosed);
    TEST_ASSERT_FALSE(r.fsm.enroll_open());
}

static void test_trigger_span_rides_msg_to_record(void) {
    // F1: gesture trigger provenance flows ControlMsg -> dispatcher -> FSM
    // -> record, converted clock-µs -> session-t (floored at session start,
    // no monotonic clamp — it points INTO the timeline, like the downbeat).
    Rig r;
    r.click.running = true;

    // Session starts at 1000 ms (first declaration); trigger hits ran
    // 2500..3920 ms on the clock -> session-t 1500..2920.
    r.dispatcher.dispatch(op(ControlMsg::Op::Bookmark), 1000 * MS);

    ControlMsg msg = op(ControlMsg::Op::GridToggle);
    msg.trigger.present = true;
    msg.trigger.t0_us = 2500 * MS;
    msg.trigger.t1_us = 3920 * MS;
    r.dispatcher.dispatch(msg, 4620 * MS);
    const CaptureRecord& open_rec = r.sink[r.sink.n - 1];
    TEST_ASSERT_TRUE(open_rec.type == RecType::GridStart);
    TEST_ASSERT_EQUAL_UINT8(1, open_rec.trig.present);
    TEST_ASSERT_EQUAL_UINT32(1500, open_rec.trig.t0);
    TEST_ASSERT_EQUAL_UINT32(2920, open_rec.trig.t1);

    // Toggle-close carries its own (later) gesture span.
    msg.trigger.t0_us = 9000 * MS;
    msg.trigger.t1_us = 10400 * MS;
    r.dispatcher.dispatch(msg, 11100 * MS);
    const CaptureRecord& close_rec = r.sink[r.sink.n - 1];
    TEST_ASSERT_TRUE(close_rec.type == RecType::GridEnd);
    TEST_ASSERT_EQUAL_UINT8(1, close_rec.trig.present);
    TEST_ASSERT_EQUAL_UINT32(8000, close_rec.trig.t0);
    TEST_ASSERT_EQUAL_UINT32(9400, close_rec.trig.t1);
}

static void test_console_declarations_carry_no_trigger_span(void) {
    Rig r;
    r.click.running = true;
    r.dispatcher.dispatch(op(ControlMsg::Op::GridStart), 1000 * MS);  // no trigger set
    const CaptureRecord& rec = r.sink[r.sink.n - 1];
    TEST_ASSERT_TRUE(rec.type == RecType::GridStart);
    TEST_ASSERT_EQUAL_UINT8(0, rec.trig.present);
}

static void test_end_session_resets_gestures(void) {
    Rig r;
    r.gestures.on_event(1000, 36, 100);
    r.gestures.on_event(1020, 49, 100);  // chord 1 of what would be a Bookmark pair

    const ControlResult res =
        r.dispatcher.dispatch(op(ControlMsg::Op::EndSession), 1100 * MS);
    TEST_ASSERT_TRUE(res.outcome == ControlOutcome::SessionEnded);

    // Without the dispatcher calling gestures.reset(), this second chord
    // (well within seq_gap_ms of chord 1) would complete a Bookmark pair.
    r.gestures.on_event(1200, 36, 100);
    r.gestures.on_event(1220, 49, 100);
    TEST_ASSERT_TRUE(r.gestures.poll(2000) == GestureDetector::Action::None);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_bookmark_dispatches_to_fsm);
    RUN_TEST(test_grid_start_refused_without_click);
    RUN_TEST(test_grid_start_snapshots_click);
    RUN_TEST(test_grid_toggle_opens_then_closes);
    RUN_TEST(test_enroll_start_labeled_is_provenance);
    RUN_TEST(test_enroll_start_no_label_is_anonymous);
    RUN_TEST(test_enroll_start_refused_without_click);
    RUN_TEST(test_enroll_end_no_span_open);
    RUN_TEST(test_enroll_toggle_always_anonymous_regardless_of_ref);
    RUN_TEST(test_trigger_span_rides_msg_to_record);
    RUN_TEST(test_console_declarations_carry_no_trigger_span);
    RUN_TEST(test_end_session_resets_gestures);
    return UNITY_END();
}
