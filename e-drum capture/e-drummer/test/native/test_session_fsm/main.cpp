// Session lifecycle semantics under fake ports: auto-start, monotonic t,
// auto-pause ctrl caching + resume delta flush, idle-timeout end, tidy
// span auto-close, cross-session controller baseline.
#include <string.h>

#include <unity.h>

#include "edrum/session_fsm.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

namespace {

struct FakeWall : hal::IWallClock {
    bool ok = true;
    bool now(DateTime& out) override {
        if (!ok) return false;
        out.year = 2026;
        out.month = 7;
        out.day = 6;
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
    bool full = false;
    bool push(const CaptureRecord& r) override {
        if (full || n >= 256) return false;
        recs[n++] = r;
        return true;
    }
    const CaptureRecord& operator[](int i) const { return recs[i]; }
};

struct Rig {
    FakeWall wall;
    FakeRandom rnd;
    VecSink sink;
    Counters c;
    SessionController::Cfg cfg;
    SessionController* fsm;

    Rig(uint32_t pause_ms = 3000, uint32_t idle_ms = 300000) {
        cfg.pause_after_ms = pause_ms;
        cfg.idle_end_ms = idle_ms;
        fsm = new SessionController(cfg, sink, wall, rnd, c);
    }
    ~Rig() { delete fsm; }
};

constexpr uint64_t MS = 1000;  // µs per ms

}  // namespace

static void test_first_event_starts_session_at_t0(void) {
    Rig r;
    TEST_ASSERT_FALSE(r.fsm->in_session());
    r.fsm->on_event(5000 * MS, 36, 100, 9);
    TEST_ASSERT_TRUE(r.fsm->in_session());
    TEST_ASSERT_EQUAL_INT(2, r.sink.n);  // SessionStart marker + event
    TEST_ASSERT_TRUE(r.sink[0].type == RecType::SessionStart);
    TEST_ASSERT_EQUAL_STRING("2026-07-06T12:00:00+08:00", r.sink[0].u.session_start.start_iso);
    TEST_ASSERT_EQUAL_UINT32(32, (uint32_t)strlen(r.sink[0].u.session_start.session_id));
    TEST_ASSERT_TRUE(r.sink[1].type == RecType::Event);
    TEST_ASSERT_EQUAL_UINT32(0, r.sink[1].t);  // anchored at first hit

    r.fsm->on_event(5250 * MS, 38, 90, 9);
    TEST_ASSERT_EQUAL_UINT32(250, r.sink[2].t);
    TEST_ASSERT_EQUAL_UINT32(1, r.c.sessions_started);
}

static void test_pause_caches_cc_and_resume_flushes_delta(void) {
    Rig r(3000, 300000);
    r.fsm->on_event(0, 36, 100, 9);
    // active play: CC writes through
    r.fsm->on_ctrl(100 * MS, MidiMsg{0xB9, 4, 10});
    TEST_ASSERT_EQUAL_INT(3, r.sink.n);
    TEST_ASSERT_TRUE(r.sink[2].type == RecType::CtrlMidi);

    // 3s of event silence -> paused; pedal wiggles cache, no lines
    r.fsm->tick(3200 * MS);
    TEST_ASSERT_TRUE(r.fsm->paused());
    r.fsm->on_ctrl(4000 * MS, MidiMsg{0xB9, 4, 50});
    r.fsm->on_ctrl(5000 * MS, MidiMsg{0xB9, 4, 88});
    TEST_ASSERT_EQUAL_INT(3, r.sink.n);  // nothing written while paused

    // resuming hit: delta (final CC value only) flushed at the hit's t, first
    r.fsm->on_event(6000 * MS, 38, 90, 9);
    TEST_ASSERT_EQUAL_INT(5, r.sink.n);
    TEST_ASSERT_TRUE(r.sink[3].type == RecType::CtrlMidi);
    TEST_ASSERT_EQUAL_HEX8(0xB9, r.sink[3].u.ctrl.msg.status);
    TEST_ASSERT_EQUAL_UINT8(88, r.sink[3].u.ctrl.msg.data2);  // final value only
    TEST_ASSERT_EQUAL_UINT32(6000, r.sink[3].t);
    TEST_ASSERT_TRUE(r.sink[4].type == RecType::Event);
    TEST_ASSERT_EQUAL_UINT32(6000, r.sink[4].t);
    TEST_ASSERT_FALSE(r.fsm->paused());

    // t counted through the pause: never rebased (invariant 6)
    TEST_ASSERT_EQUAL_UINT32(0, r.c.ring_drops);
}

static void test_session_starts_with_full_controller_baseline(void) {
    Rig r;
    // pre-session pedal state (cached, no session started)
    r.fsm->on_ctrl(1000 * MS, MidiMsg{0xB9, 4, 30});
    TEST_ASSERT_FALSE(r.fsm->in_session());
    TEST_ASSERT_EQUAL_INT(0, r.sink.n);

    r.fsm->on_event(2000 * MS, 36, 100, 9);
    // SessionStart, baseline CC4=30 at t=0, then the event
    TEST_ASSERT_EQUAL_INT(3, r.sink.n);
    TEST_ASSERT_TRUE(r.sink[1].type == RecType::CtrlMidi);
    TEST_ASSERT_EQUAL_UINT8(4, r.sink[1].u.ctrl.msg.data1);
    TEST_ASSERT_EQUAL_UINT8(30, r.sink[1].u.ctrl.msg.data2);
    TEST_ASSERT_EQUAL_UINT32(0, r.sink[1].t);

    // second session gets the baseline again (self-contained files)
    r.fsm->end_session(10000 * MS);
    r.fsm->on_event(20000 * MS, 36, 90, 9);
    bool baseline_again = false;
    for (int i = 0; i < r.sink.n; ++i) {
        if (r.sink[i].type == RecType::CtrlMidi && r.sink[i].t == 0 && i > 4) {
            baseline_again = true;
        }
    }
    TEST_ASSERT_TRUE(baseline_again);
}

static void test_idle_timeout_ends_session_and_closes_spans(void) {
    Rig r(3000, 60000);  // 1 min idle end
    r.fsm->on_event(0, 36, 100, 9);
    r.fsm->grid_start(1000 * MS, 120, 4, 1000 * MS);
    TEST_ASSERT_TRUE(r.fsm->grid_open());

    r.fsm->tick(30000 * MS);
    TEST_ASSERT_TRUE(r.fsm->in_session());
    r.fsm->tick(62000 * MS);
    TEST_ASSERT_FALSE(r.fsm->in_session());

    // grid_end then session_end, tidy
    TEST_ASSERT_TRUE(r.sink[r.sink.n - 2].type == RecType::GridEnd);
    TEST_ASSERT_TRUE(r.sink[r.sink.n - 1].type == RecType::SessionEnd);
    TEST_ASSERT_EQUAL_UINT32(62000, r.sink[r.sink.n - 1].t);
    TEST_ASSERT_EQUAL_UINT32(1, r.c.sessions_ended);
}

static void test_declaration_starts_session_and_downbeat_clamped(void) {
    Rig r;
    // grid declared before any hit: session starts; downbeat anchor clamps
    // to session start (never negative)
    r.fsm->grid_start(5000 * MS, 96, 4, 4000 * MS);
    TEST_ASSERT_TRUE(r.fsm->in_session());
    TEST_ASSERT_EQUAL_INT(2, r.sink.n);
    TEST_ASSERT_TRUE(r.sink[1].type == RecType::GridStart);
    TEST_ASSERT_EQUAL_UINT32(0, r.sink[1].u.grid.downbeat_t);
    TEST_ASSERT_EQUAL_UINT16(96, r.sink[1].u.grid.bpm);

    // future downbeat maps to session-relative ms
    r.fsm->grid_end(6000 * MS);
    r.fsm->grid_start(7000 * MS, 96, 4, 7500 * MS);
    TEST_ASSERT_EQUAL_UINT32(2500, r.sink[r.sink.n - 1].u.grid.downbeat_t);
}

static void test_unmatched_ends_refused(void) {
    Rig r;
    TEST_ASSERT_FALSE(r.fsm->grid_end(1000 * MS));
    TEST_ASSERT_FALSE(r.fsm->enroll_end(1000 * MS));
    TEST_ASSERT_EQUAL_INT(0, r.sink.n);  // nothing written
}

static void test_nosession_ctrl_policy(void) {
    Rig r;
    r.fsm->on_ctrl(0, MidiMsg{0x89, 36, 64});  // orphan note_off: dropped+counted
    TEST_ASSERT_EQUAL_UINT32(1, r.c.ctrl_nosession_dropped);
    r.fsm->on_sysex(0, nullptr, 0, false);
    TEST_ASSERT_EQUAL_UINT32(2, r.c.ctrl_nosession_dropped);
    TEST_ASSERT_FALSE(r.fsm->in_session());
}

static void test_ring_full_counts_drops(void) {
    Rig r;
    r.sink.full = true;
    r.fsm->on_event(0, 36, 100, 9);
    TEST_ASSERT_TRUE(r.c.ring_drops > 0);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_first_event_starts_session_at_t0);
    RUN_TEST(test_pause_caches_cc_and_resume_flushes_delta);
    RUN_TEST(test_session_starts_with_full_controller_baseline);
    RUN_TEST(test_idle_timeout_ends_session_and_closes_spans);
    RUN_TEST(test_declaration_starts_session_and_downbeat_clamped);
    RUN_TEST(test_unmatched_ends_refused);
    RUN_TEST(test_nosession_ctrl_policy);
    RUN_TEST(test_ring_full_counts_drops);
    return UNITY_END();
}
