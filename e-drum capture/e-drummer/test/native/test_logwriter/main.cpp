// Storage-policy tests: LogWriter against an in-memory IStorage + FakeClock.
// Proves the whole storage path (marker -> file open -> meta -> lines ->
// sync policy -> close) with zero hardware, including a byte-compare of a
// full session file against the brain's golden fixture.
#include <string.h>

#include <unity.h>

#include "edrum/logwriter.h"
#include "../test_conformance/fixtures_data.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

namespace {

struct FakeClock : hal::IClock {
    uint64_t t = 0;
    uint64_t now_us() override { return t; }
};

struct MemStorage : hal::IStorage {
    char data[16384];
    size_t len = 0;
    size_t synced_len = 0;
    bool is_open = false;
    int creates = 0, syncs = 0, closes = 0;
    bool fail_create = false;
    bool fail_append = false;  // simulates a yanked/full/corrupt card
    char last_path[96] = {0};

    bool exists(const char*) override { return false; }
    int read_file(const char*, char*, size_t) override { return -1; }
    bool create(const char* path) override {
        if (fail_create) return false;
        strncpy(last_path, path, sizeof(last_path) - 1);
        is_open = true;
        ++creates;
        return true;
    }
    bool append(const uint8_t* d, size_t n) override {
        if (fail_append || !is_open || len + n > sizeof(data)) return false;
        memcpy(data + len, d, n);
        len += n;
        return true;
    }
    bool sync() override {
        if (!is_open) return false;
        synced_len = len;
        ++syncs;
        return true;
    }
    void close() override {
        is_open = false;
        ++closes;
    }
};

CaptureRecord start_marker(const char* id, const char* iso) {
    CaptureRecord r{};
    r.type = RecType::SessionStart;
    r.t = 0;
    strncpy(r.u.session_start.session_id, id, kSessionIdLen);
    strncpy(r.u.session_start.start_iso, iso, kIsoMax);
    return r;
}

CaptureRecord event(uint32_t t, uint8_t note, uint8_t vel) {
    CaptureRecord r{};
    r.type = RecType::Event;
    r.t = t;
    r.u.event = EventP{note, vel, 9};
    return r;
}

CaptureRecord session_end(uint32_t t) {
    CaptureRecord r{};
    r.type = RecType::SessionEnd;
    r.t = t;
    return r;
}

MetaStatic td02k_meta() {
    MetaStatic ms{};
    strcpy(ms.kit_profile_id, "td02k");
    strcpy(ms.user_id, "local");
    ms.has_calibration = false;
    return ms;
}

}  // namespace

static void test_minimal_fixture_through_storage_path(void) {
    FakeClock clk;
    MemStorage st;
    Counters c;
    LogWriter::Cfg cfg{"/sessions", 4096, 250};
    LogWriter w(cfg, st, clk, td02k_meta(), c);

    w.handle(start_marker("11111111aaaa4bbb8ccc000000000001", "2026-07-06T12:00:00+08:00"));
    TEST_ASSERT_TRUE(w.file_open());
    TEST_ASSERT_EQUAL_STRING("/sessions/20260706T120000_11111111.jsonl", st.last_path);
    TEST_ASSERT_TRUE(st.synced_len > 0);  // meta durable immediately

    w.handle(event(100, 36, 96));
    w.handle(event(600, 38, 104));
    w.handle(event(1100, 42, 72));
    w.handle(session_end(1500));

    TEST_ASSERT_FALSE(w.file_open());
    TEST_ASSERT_EQUAL_INT(1, st.closes);
    st.data[st.len] = '\0';
    TEST_ASSERT_EQUAL_STRING(kFixtureMinimal, st.data);       // byte-identical file
    TEST_ASSERT_EQUAL_UINT32(st.len, st.synced_len);          // fully durable at close
    TEST_ASSERT_EQUAL_UINT32(0, c.storage_write_errors);
    TEST_ASSERT_EQUAL_UINT32(0, c.serialize_errors);
}

static void test_sync_policy_bytes_and_time(void) {
    FakeClock clk;
    MemStorage st;
    Counters c;
    LogWriter::Cfg cfg{"/sessions", 200, 250};  // 200B or 250ms
    LogWriter w(cfg, st, clk, td02k_meta(), c);

    w.handle(start_marker("11111111aaaa4bbb8ccc000000000001", "2026-07-06T12:00:00+08:00"));
    const int syncs_after_meta = st.syncs;

    // ~60B/line: third event crosses the 200B budget
    w.handle(event(10, 36, 96));
    w.handle(event(20, 38, 96));
    TEST_ASSERT_EQUAL_INT(syncs_after_meta, st.syncs);
    w.handle(event(30, 42, 96));
    w.handle(event(40, 44, 96));
    TEST_ASSERT_TRUE(st.syncs > syncs_after_meta);

    // time-based: one small line, then idle past 250ms
    const int syncs_now = st.syncs;
    w.handle(event(50, 36, 96));
    w.idle();
    TEST_ASSERT_EQUAL_INT(syncs_now, st.syncs);  // not due yet
    clk.t += 300000;                              // +300ms
    w.idle();
    TEST_ASSERT_EQUAL_INT(syncs_now + 1, st.syncs);
    w.idle();                                     // nothing unsynced: no-op
    TEST_ASSERT_EQUAL_INT(syncs_now + 1, st.syncs);
}

static void test_records_without_session_are_counted(void) {
    FakeClock clk;
    MemStorage st;
    Counters c;
    LogWriter::Cfg cfg{"/sessions", 4096, 250};
    LogWriter w(cfg, st, clk, td02k_meta(), c);

    // a stray record before ANY session is a producer bug, not fail-soft
    w.handle(event(10, 36, 96));
    TEST_ASSERT_EQUAL_UINT32(1, c.storage_write_errors);
    TEST_ASSERT_EQUAL_UINT32(0, c.storage_discarded);
    TEST_ASSERT_EQUAL_UINT32(0, st.len);
    TEST_ASSERT_FALSE(w.failed());
}

// Fail-soft policy (decision 5): a card that can't open a session file
// latches failed(); records DISCARD gracefully (counted once each, no
// per-record error spam, no further SD hammering) and the next session
// retries — a re-seated card resumes persisting with no reboot.
static void test_failed_create_latches_and_discards(void) {
    FakeClock clk;
    MemStorage st;
    Counters c;
    LogWriter::Cfg cfg{"/sessions", 4096, 250};
    LogWriter w(cfg, st, clk, td02k_meta(), c);

    st.fail_create = true;
    w.handle(start_marker("22222222aaaa4bbb8ccc000000000002", "2026-07-06T12:00:00+08:00"));
    TEST_ASSERT_FALSE(w.file_open());
    TEST_ASSERT_TRUE(w.failed());
    TEST_ASSERT_EQUAL_UINT32(1, c.storage_write_errors);  // the create itself

    w.handle(event(20, 36, 96));
    w.handle(event(30, 38, 96));
    w.handle(session_end(40));
    TEST_ASSERT_EQUAL_UINT32(1, c.storage_write_errors);  // no error spam
    TEST_ASSERT_EQUAL_UINT32(3, c.storage_discarded);     // graceful discard

    // card re-seated: the next session persists normally
    st.fail_create = false;
    w.handle(start_marker("33333333aaaa4bbb8ccc000000000003", "2026-07-06T12:00:00+08:00"));
    TEST_ASSERT_TRUE(w.file_open());
    TEST_ASSERT_FALSE(w.failed());
    w.handle(event(50, 36, 96));
    TEST_ASSERT_EQUAL_UINT32(3, c.storage_discarded);  // discards stopped
}

// Mid-session card death: repeated append failures latch fail-soft instead
// of stalling the storage task on a dead card forever.
static void test_append_failures_latch_fail_soft(void) {
    FakeClock clk;
    MemStorage st;
    Counters c;
    LogWriter::Cfg cfg{"/sessions", 4096, 250};
    LogWriter w(cfg, st, clk, td02k_meta(), c);

    w.handle(start_marker("11111111aaaa4bbb8ccc000000000001", "2026-07-06T12:00:00+08:00"));
    w.handle(event(100, 36, 96));
    TEST_ASSERT_TRUE(w.file_open());

    st.fail_append = true;  // card yanked mid-session
    w.handle(event(200, 38, 96));
    w.handle(event(300, 40, 96));
    TEST_ASSERT_TRUE(w.file_open());  // still trying (< 3 consecutive)
    w.handle(event(400, 42, 96));     // third consecutive failure: latch
    TEST_ASSERT_FALSE(w.file_open());
    TEST_ASSERT_TRUE(w.failed());
    TEST_ASSERT_EQUAL_UINT32(3, c.storage_write_errors);

    w.handle(event(500, 36, 96));  // discarded, not attempted
    TEST_ASSERT_EQUAL_UINT32(1, c.storage_discarded);
    TEST_ASSERT_EQUAL_UINT32(3, c.storage_write_errors);

    // next session retries the card
    st.fail_append = false;
    w.handle(start_marker("55555555aaaa4bbb8ccc000000000005", "2026-07-06T14:00:00+08:00"));
    TEST_ASSERT_TRUE(w.file_open());
    TEST_ASSERT_FALSE(w.failed());
}

static void test_open_name_tracks_the_open_session(void) {
    FakeClock clk;
    MemStorage st;
    Counters c;
    LogWriter::Cfg cfg{"/sessions", 4096, 250};
    LogWriter w(cfg, st, clk, td02k_meta(), c);

    TEST_ASSERT_NULL(w.open_name());
    w.handle(start_marker("11111111aaaa4bbb8ccc000000000001", "2026-07-06T12:00:00+08:00"));
    TEST_ASSERT_EQUAL_STRING("20260706T120000_11111111.jsonl", w.open_name());
    w.handle(session_end(100));
    TEST_ASSERT_NULL(w.open_name());  // sync LIST may now include it
}

static void test_two_sessions_sequential(void) {
    FakeClock clk;
    MemStorage st;
    Counters c;
    LogWriter::Cfg cfg{"/sessions", 4096, 250};
    LogWriter w(cfg, st, clk, td02k_meta(), c);

    w.handle(start_marker("11111111aaaa4bbb8ccc000000000001", "2026-07-06T12:00:00+08:00"));
    w.handle(event(100, 36, 96));
    w.handle(session_end(200));
    TEST_ASSERT_EQUAL_INT(1, st.closes);

    w.handle(start_marker("44444444aaaa4bbb8ccc000000000004", "2026-07-06T13:00:00+08:00"));
    TEST_ASSERT_TRUE(w.file_open());
    TEST_ASSERT_EQUAL_STRING("/sessions/20260706T130000_44444444.jsonl", st.last_path);
    TEST_ASSERT_EQUAL_INT(2, st.creates);
    TEST_ASSERT_EQUAL_UINT32(2, c.files_opened);
}

static void test_monotonic_violation_counted_not_fatal(void) {
    FakeClock clk;
    MemStorage st;
    Counters c;
    LogWriter::Cfg cfg{"/sessions", 4096, 250};
    LogWriter w(cfg, st, clk, td02k_meta(), c);

    w.handle(start_marker("11111111aaaa4bbb8ccc000000000001", "2026-07-06T12:00:00+08:00"));
    w.handle(event(500, 36, 96));
    w.handle(event(400, 38, 96));  // t goes backwards: bug trap fires
    TEST_ASSERT_EQUAL_UINT32(1, c.storage_write_errors);
    TEST_ASSERT_EQUAL_UINT32(3, c.storage_lines);  // meta + both events still written
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_minimal_fixture_through_storage_path);
    RUN_TEST(test_sync_policy_bytes_and_time);
    RUN_TEST(test_records_without_session_are_counted);
    RUN_TEST(test_failed_create_latches_and_discards);
    RUN_TEST(test_append_failures_latch_fail_soft);
    RUN_TEST(test_open_name_tracks_the_open_session);
    RUN_TEST(test_two_sessions_sequential);
    RUN_TEST(test_monotonic_violation_counted_not_fatal);
    return UNITY_END();
}
