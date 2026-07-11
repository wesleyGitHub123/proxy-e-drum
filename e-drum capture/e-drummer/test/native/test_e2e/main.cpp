// End-to-end pipeline test — the firmware analog of the brain's Slice 1
// (phase0-plan "Vertical slices"): scripted USB-MIDI packets flow through
// the REAL pipeline — UsbMidiParser -> classify -> SessionController ->
// SPSC ring -> LogWriter -> in-memory IStorage — and the resulting session
// file is checked line by line. No hardware, no tasks; time is scripted.
//
// This is the test that proves the subsystems compose: every seam crossed
// here is the same seam the ESP32 tasks cross at runtime.
#include <string.h>

#include <unity.h>

#include "edrum/classify.h"
#include "edrum/logwriter.h"
#include "edrum/ring.h"
#include "edrum/session_fsm.h"
#include "edrum/usbmidi.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

namespace {

struct FakeClock : hal::IClock {
    uint64_t t = 0;
    uint64_t now_us() override { return t; }
};

struct FakeWall : hal::IWallClock {
    bool now(DateTime& out) override {
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
        memset(buf, 0xAB, len);
    }
};

struct MemStorage : hal::IStorage {
    char data[16384];
    size_t len = 0;
    bool is_open = false;
    bool exists(const char*) override { return false; }
    int read_file(const char*, char*, size_t) override { return -1; }
    bool create(const char*) override {
        is_open = true;
        return true;
    }
    bool append(const uint8_t* d, size_t n) override {
        if (!is_open || len + n > sizeof(data)) return false;
        memcpy(data + len, d, n);
        len += n;
        return true;
    }
    bool sync() override { return is_open; }
    void close() override { is_open = false; }
};

struct RingSink : RecordSink {
    SpscRing<CaptureRecord, 512>& ring;
    explicit RingSink(SpscRing<CaptureRecord, 512>& r) : ring(r) {}
    bool push(const CaptureRecord& rec) override { return ring.push(rec); }
};

// The capture glue in miniature: parser handler -> classify -> FSM.
struct Glue : UsbMidiParser::Handler {
    SessionController& fsm;
    FakeClock& clk;
    Counters& c;
    Glue(SessionController& f, FakeClock& k, Counters& cnt) : fsm(f), clk(k), c(cnt) {}

    void on_midi(const MidiMsg& m) override {
        const uint64_t t = clk.now_us();  // arrival stamp
        switch (classify(m)) {
            case MsgClass::Event:
                fsm.on_event(t, m.data1, m.data2, m.channel());
                break;
            case MsgClass::Ctrl:
                fsm.on_ctrl(t, m);
                break;
            case MsgClass::DropRealtime:
                c.realtime_dropped++;
                break;
        }
    }
    void on_sysex(const uint8_t* d, uint8_t len, bool tr) override {
        fsm.on_sysex(clk.now_us(), d, len, tr);
    }
};

}  // namespace

static void test_full_pipeline(void) {
    FakeClock clk;
    FakeWall wall;
    FakeRandom rnd;
    Counters c;
    SpscRing<CaptureRecord, 512> ring;
    RingSink sink(ring);

    SessionController::Cfg fcfg;
    fcfg.pause_after_ms = 3000;
    fcfg.idle_end_ms = 300000;
    SessionController fsm(fcfg, sink, wall, rnd, c);

    MemStorage storage;
    MetaStatic ms{};
    strcpy(ms.kit_profile_id, "td02k");
    strcpy(ms.user_id, "local");
    LogWriter::Cfg wcfg{"/sessions", 4096, 250};
    LogWriter writer(wcfg, storage, clk, ms, c);

    Glue glue(fsm, clk, c);
    UsbMidiParser parser(glue);

    auto pump = [&]() {  // storage task in miniature
        CaptureRecord rec;
        while (ring.pop(rec)) writer.handle(rec);
    };
    auto note_on = [&](uint64_t t_us, uint8_t note, uint8_t vel) {
        clk.t = t_us;
        const uint8_t pkt[4] = {0x09, 0x99, note, vel};
        parser.feed_packet(pkt);
    };
    auto note_off = [&](uint64_t t_us, uint8_t note) {
        clk.t = t_us;
        const uint8_t pkt[4] = {0x08, 0x89, note, 64};
        parser.feed_packet(pkt);
    };

    // active sensing before anything: dropped, no session
    clk.t = 500000;
    const uint8_t sense[4] = {0x0F, 0xFE, 0, 0};
    parser.feed_packet(sense);
    pump();
    TEST_ASSERT_FALSE(fsm.in_session());
    TEST_ASSERT_EQUAL_UINT32(1, c.realtime_dropped);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)storage.len);

    // hi-hat pedal moves pre-session: cached, still no session
    clk.t = 800000;
    const uint8_t cc[4] = {0x0B, 0xB9, 4, 90};
    parser.feed_packet(cc);
    pump();
    TEST_ASSERT_FALSE(fsm.in_session());

    // first hit at wall-time t: session starts, baseline flushes
    note_on(1000000, 36, 96);
    note_off(1030000, 36);
    note_on(1500000, 38, 104);
    pump();
    TEST_ASSERT_TRUE(fsm.in_session());
    TEST_ASSERT_TRUE(writer.file_open());

    // click-declared grid + bookmark via FSM (console/gesture path)
    fsm.grid_start(2000000, 120, 4, 2000000);
    note_on(2500000, 42, 72);
    fsm.grid_end(3000000);
    fsm.bookmark(3200000);
    pump();

    // explicit end
    fsm.end_session(4000000);
    pump();
    TEST_ASSERT_FALSE(writer.file_open());

    storage.data[storage.len] = '\0';

    // line-by-line check (session_id is random-derived, so meta is matched
    // by parts rather than whole)
    char* file = storage.data;
    char* lines[32];
    int n = 0;
    for (char* p = file; *p && n < 32;) {
        lines[n++] = p;
        char* nl = strchr(p, '\n');
        TEST_ASSERT_NOT_NULL(nl);
        *nl = '\0';
        p = nl + 1;
    }
    TEST_ASSERT_EQUAL_INT(10, n);
    TEST_ASSERT_NOT_NULL(strstr(lines[0], "\"type\":\"meta\",\"schema_version\":1"));
    TEST_ASSERT_NOT_NULL(strstr(lines[0], "\"start_iso\":\"2026-07-06T12:00:00+08:00\""));
    TEST_ASSERT_NOT_NULL(strstr(lines[0], "\"kit_profile_id\":\"td02k\""));
    TEST_ASSERT_NOT_NULL(strstr(lines[0], "\"calibration_offset_ms\":null"));
    // baseline CC flushed at t=0 (state captured pre-session)
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ctrl\",\"t\":0,\"msg\":{\"type\":\"control_change\",\"channel\":9,\"control\":4,\"value\":90}}",
        lines[1]);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"event\",\"t\":0,\"note\":36,\"velocity\":96,\"channel\":9}",
                             lines[2]);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"ctrl\",\"t\":30,\"msg\":{\"type\":\"note_off\",\"channel\":9,\"note\":36,\"velocity\":64}}",
        lines[3]);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"event\",\"t\":500,\"note\":38,\"velocity\":104,\"channel\":9}",
                             lines[4]);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"grid_start\",\"t\":1000,\"bpm\":120,\"subdiv\":4,\"downbeat_t\":1000}", lines[5]);
    // (event at t=1500 inside the grid)
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"event\",\"t\":1500,\"note\":42,\"velocity\":72,\"channel\":9}",
                             lines[6]);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"grid_end\",\"t\":2000}", lines[7]);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"bookmark\",\"t\":2200}", lines[8]);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"session_end\",\"t\":3000}", lines[9]);

    TEST_ASSERT_EQUAL_UINT32(0, c.ring_drops);
    TEST_ASSERT_EQUAL_UINT32(0, c.serialize_errors);
    TEST_ASSERT_EQUAL_UINT32(0, c.storage_write_errors);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_full_pipeline);
    return UNITY_END();
}
