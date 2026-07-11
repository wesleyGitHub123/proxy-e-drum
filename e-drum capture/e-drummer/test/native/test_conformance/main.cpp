// Golden-fixture byte conformance — the load-bearing test of the firmware
// (phase0-plan "Golden fixtures": "this corpus IS the conformance suite the
// ESP32 firmware must satisfy byte-for-byte"). Reconstructs each producible
// fixture through the firmware serializer and memcmp's the whole file.
//
// Expected bytes come from fixtures_data.h, GENERATED from the brain's
// tests/fixtures by tools/gen_fixture_header.py — never hand-copied.
#include <string.h>

#include <unity.h>

#include "edrum/serialize.h"
#include "fixtures_data.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

namespace {

struct FileBuilder {
    char data[8192];
    size_t len = 0;

    void meta(const char* session_id, const char* start_iso) {
        SessionStartP s{};
        strncpy(s.session_id, session_id, kSessionIdLen);
        strncpy(s.start_iso, start_iso, kIsoMax);
        MetaStatic ms{};
        strcpy(ms.kit_profile_id, "td02k");
        strcpy(ms.user_id, "local");
        ms.has_calibration = false;
        const size_t n = meta_line(s, ms, data + len, sizeof(data) - len);
        TEST_ASSERT_TRUE_MESSAGE(n > 0, "meta_line failed");
        len += n;
    }

    void add(const CaptureRecord& r) {
        const size_t n = record_line(r, data + len, sizeof(data) - len);
        TEST_ASSERT_TRUE_MESSAGE(n > 0, "record_line failed");
        len += n;
    }

    void event(uint32_t t, uint8_t note, uint8_t vel) {
        CaptureRecord r{};
        r.type = RecType::Event;
        r.t = t;
        r.u.event = EventP{note, vel, 9};
        add(r);
    }

    void end(uint32_t t) {
        CaptureRecord r{};
        r.type = RecType::SessionEnd;
        r.t = t;
        add(r);
    }

    void expect(const char* fixture, const char* name) {
        data[len] = '\0';
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)strlen(fixture), (uint32_t)len, name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(fixture, data, name);
    }
};

}  // namespace

static void test_fixture_minimal(void) {
    FileBuilder f;
    f.meta("11111111aaaa4bbb8ccc000000000001", "2026-07-06T12:00:00+08:00");
    f.event(100, 36, 96);
    f.event(600, 38, 104);
    f.event(1100, 42, 72);
    f.end(1500);
    f.expect(kFixtureMinimal, "minimal.jsonl");
}

static void test_fixture_declarations(void) {
    FileBuilder f;
    f.meta("22222222aaaa4bbb8ccc000000000002", "2026-07-06T12:00:00+08:00");

    CaptureRecord r{};
    r.type = RecType::CtrlMidi;
    r.t = 50;
    r.u.ctrl.msg = MidiMsg{0xB9, 4, 88};  // hi-hat pedal CC4
    f.add(r);

    f.event(500, 42, 60);

    r = CaptureRecord{};
    r.type = RecType::GridStart;
    r.t = 1000;
    r.u.grid = GridP{120, 4, 1000};
    f.add(r);

    f.event(1000, 36, 100);
    f.event(1500, 38, 95);
    f.event(2000, 36, 98);

    r = CaptureRecord{};
    r.type = RecType::GridEnd;
    r.t = 3000;
    f.add(r);

    r = CaptureRecord{};
    r.type = RecType::Bookmark;
    r.t = 3500;
    f.add(r);

    r = CaptureRecord{};
    r.type = RecType::EnrollStart;
    r.t = 4000;
    strcpy(r.u.enroll.profile_ref, "basic-rock");
    r.u.enroll.bpm = 120;
    r.u.enroll.subdiv = 4;
    r.u.enroll.downbeat_t = 4000;
    f.add(r);

    f.event(4000, 36, 100);
    f.event(4500, 38, 90);

    r = CaptureRecord{};
    r.type = RecType::EnrollEnd;
    r.t = 6000;
    f.add(r);

    r = CaptureRecord{};
    r.type = RecType::CtrlMidi;
    r.t = 6200;
    r.u.ctrl.msg = MidiMsg{0x89, 42, 64};  // note_off
    f.add(r);

    f.end(6500);
    f.expect(kFixtureDeclarations, "declarations.jsonl");
}

static void test_fixture_warmup_no_grid(void) {
    FileBuilder f;
    f.meta("33333333aaaa4bbb8ccc000000000003", "2026-07-06T12:00:00+08:00");
    const uint8_t notes[2] = {38, 42};
    for (int i = 0; i < 8; ++i) {
        f.event(100 + 250 * (uint32_t)i, notes[i % 2], (uint8_t)(70 + i));
    }
    f.end(2200);
    f.expect(kFixtureWarmupNoGrid, "warmup_no_grid.jsonl");
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_fixture_minimal);
    RUN_TEST(test_fixture_declarations);
    RUN_TEST(test_fixture_warmup_no_grid);
    return UNITY_END();
}
