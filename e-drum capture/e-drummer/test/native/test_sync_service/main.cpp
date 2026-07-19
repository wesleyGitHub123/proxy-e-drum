// SyncService against a fake archive + capturing sink: every request path
// of the session-archive protocol (capture spec §13) natively covered —
// version refusal, closed-sessions-only LIST, ranged FETCH with EOF
// semantics, chained CRC, explicit StorageFailed (fail-soft decision 5),
// and BYE ending the framed session. This is the same discipline that
// covers ControlDispatcher: the protocol is provable without a transport.
#include <string.h>

#include <unity.h>

#include "edrum/frame.h"
#include "edrum/sync_service.h"

using namespace edrum;

void setUp() {}
void tearDown() {}

namespace {

struct FakeArchive : hal::ISessionArchive {
    struct F {
        const char* name;
        const uint8_t* data;
        uint32_t size;
    };
    F files[4];
    int nfiles = 0;
    bool fail = false;

    int list(hal::SessionInfo* out, size_t cap) override {
        if (fail) return -1;
        int n = 0;
        for (int i = 0; i < nfiles && n < (int)cap; ++i) {
            strcpy(out[n].name, files[i].name);
            out[n].size = files[i].size;
            ++n;
        }
        return n;
    }

    int64_t size_of(const char* name) override {
        if (fail) return -1;
        for (int i = 0; i < nfiles; ++i) {
            if (strcmp(files[i].name, name) == 0) return files[i].size;
        }
        return -1;
    }

    int read(const char* name, uint32_t offset, uint8_t* buf, uint32_t len) override {
        if (fail) return -1;
        for (int i = 0; i < nfiles; ++i) {
            if (strcmp(files[i].name, name) != 0) continue;
            if (offset >= files[i].size) return 0;
            uint32_t n = files[i].size - offset;
            if (n > len) n = len;
            memcpy(buf, files[i].data + offset, n);
            return (int)n;
        }
        return -1;
    }

    bool remove(const char* name) override {
        if (fail) return false;
        for (int i = 0; i < nfiles; ++i) {
            if (strcmp(files[i].name, name) != 0) continue;
            for (int j = i; j + 1 < nfiles; ++j) files[j] = files[j + 1];
            --nfiles;
            return true;
        }
        return false;
    }
};

struct CaptureSink : FrameSink {
    uint8_t frames[8][600];
    size_t lens[8] = {0};
    int count = 0;

    void emit(const uint8_t* payload, size_t len) override {
        if (count < 8 && len <= sizeof(frames[0])) {
            memcpy(frames[count], payload, len);
            lens[count] = len;
            ++count;
        }
    }
    void clear() { count = 0; }
    uint8_t type(int i) const { return frames[i][1]; }
    uint8_t err_code(int i) const { return frames[i][2]; }
};

uint8_t g_file_a[1000];
uint8_t g_file_b[64];

void fill_files(FakeArchive& ar) {
    for (size_t i = 0; i < sizeof(g_file_a); ++i) g_file_a[i] = (uint8_t)(i * 7 + 1);
    for (size_t i = 0; i < sizeof(g_file_b); ++i) g_file_b[i] = (uint8_t)(0x30 + i % 10);
    ar.files[0] = {"20260706T120000_11111111.jsonl", g_file_a, sizeof(g_file_a)};
    ar.files[1] = {"20260707T090000_22222222.jsonl", g_file_b, sizeof(g_file_b)};
    ar.nfiles = 2;
}

SyncService::Cfg cfg() {
    SyncService::Cfg c;
    c.proto_version = sync::kProtoVersion;
    c.schema_version = 2;
    c.fw_build = "test-build";
    return c;
}

size_t hello(uint8_t* p, const char* iso) {
    p[0] = sync::kChannelSync;
    p[1] = (uint8_t)sync::Type::Hello;
    p[2] = sync::kProtoVersion;
    const size_t tl = (iso != nullptr) ? strlen(iso) : 0;
    p[3] = (uint8_t)tl;
    if (tl > 0) memcpy(p + 4, iso, tl);
    return 4 + tl;
}

size_t named_req(uint8_t* p, sync::Type t, const char* name, uint32_t offset, uint16_t len,
                 const uint16_t* seed = nullptr) {
    p[0] = sync::kChannelSync;
    p[1] = (uint8_t)t;
    const size_t nl = strlen(name);
    p[2] = (uint8_t)nl;
    memcpy(p + 3, name, nl);
    size_t i = 3 + nl;
    p[i++] = (uint8_t)(offset & 0xFF);
    p[i++] = (uint8_t)((offset >> 8) & 0xFF);
    p[i++] = (uint8_t)((offset >> 16) & 0xFF);
    p[i++] = (uint8_t)((offset >> 24) & 0xFF);
    p[i++] = (uint8_t)(len & 0xFF);
    p[i++] = (uint8_t)(len >> 8);
    if (seed != nullptr) {
        p[i++] = (uint8_t)(*seed & 0xFF);
        p[i++] = (uint8_t)(*seed >> 8);
    }
    return i;
}

size_t delete_req(uint8_t* p, const char* name) {
    p[0] = sync::kChannelSync;
    p[1] = (uint8_t)sync::Type::Delete;
    const size_t nl = strlen(name);
    p[2] = (uint8_t)nl;
    memcpy(p + 3, name, nl);
    return 3 + nl;
}

}  // namespace

static void test_hello_ok_carries_versions_storage_and_time(void) {
    FakeArchive ar;
    fill_files(ar);
    SyncService svc(cfg(), ar);
    svc.set_storage_ok(true);
    CaptureSink out;

    uint8_t req[64];
    const size_t n = hello(req, "2026-07-12T10:30:00+08:00");
    const SyncService::Result r = svc.handle(req, n, out);

    TEST_ASSERT_EQUAL_INT(1, out.count);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::HelloOk, out.type(0));
    TEST_ASSERT_EQUAL_UINT8(sync::kProtoVersion, out.frames[0][2]);
    TEST_ASSERT_EQUAL_UINT8(2, out.frames[0][3]);  // schema_version
    TEST_ASSERT_EQUAL_UINT8(1, out.frames[0][4]);  // storage_ok
    TEST_ASSERT_EQUAL_UINT8(strlen("test-build"), out.frames[0][5]);
    TEST_ASSERT_EQUAL_MEMORY("test-build", &out.frames[0][6], strlen("test-build"));

    TEST_ASSERT_TRUE(r.time_requested);
    TEST_ASSERT_EQUAL_INT16(2026, r.host_time.year);
    TEST_ASSERT_EQUAL_UINT8(10, r.host_time.hour);
    TEST_ASSERT_EQUAL_INT16(480, r.host_time.tz_offset_min);
    TEST_ASSERT_FALSE(r.session_over);
}

static void test_hello_version_mismatch_refused(void) {
    FakeArchive ar;
    SyncService svc(cfg(), ar);
    CaptureSink out;

    uint8_t req[8] = {sync::kChannelSync, (uint8_t)sync::Type::Hello,
                      (uint8_t)(sync::kProtoVersion + 1), 0};
    svc.handle(req, 4, out);
    TEST_ASSERT_EQUAL_INT(1, out.count);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::Err, out.type(0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)sync::Err::UnsupportedVersion, out.err_code(0));
}

static void test_list_excludes_open_session(void) {
    FakeArchive ar;
    fill_files(ar);
    SyncService svc(cfg(), ar);
    svc.set_storage_ok(true);
    svc.set_open_session("20260707T090000_22222222.jsonl");
    CaptureSink out;

    const uint8_t req[2] = {sync::kChannelSync, (uint8_t)sync::Type::List};
    svc.handle(req, 2, out);

    TEST_ASSERT_EQUAL_INT(2, out.count);  // one ITEM + LIST_END
    TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::Item, out.type(0));
    const uint8_t nlen = out.frames[0][2];
    TEST_ASSERT_EQUAL_UINT8(strlen("20260706T120000_11111111.jsonl"), nlen);
    TEST_ASSERT_EQUAL_MEMORY("20260706T120000_11111111.jsonl", &out.frames[0][3], nlen);
    const uint8_t* szp = &out.frames[0][3 + nlen];
    const uint32_t size = (uint32_t)szp[0] | ((uint32_t)szp[1] << 8) | ((uint32_t)szp[2] << 16) |
                          ((uint32_t)szp[3] << 24);
    TEST_ASSERT_EQUAL_UINT32(sizeof(g_file_a), size);

    TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::ListEnd, out.type(1));
    TEST_ASSERT_EQUAL_UINT16(1, (uint16_t)(out.frames[1][2] | (out.frames[1][3] << 8)));

    // no open session: both listed
    out.clear();
    svc.set_open_session(nullptr);
    svc.handle(req, 2, out);
    TEST_ASSERT_EQUAL_INT(3, out.count);
}

static void test_storage_failed_is_explicit_on_the_wire(void) {
    FakeArchive ar;
    fill_files(ar);
    SyncService svc(cfg(), ar);
    CaptureSink out;

    // decision 5: composition says storage is down -> ERR StorageFailed,
    // never a hang, for every archive-touching request
    svc.set_storage_ok(false);
    const uint8_t list_req[2] = {sync::kChannelSync, (uint8_t)sync::Type::List};
    uint8_t req[64];
    svc.handle(list_req, 2, out);
    const size_t fn = named_req(req, sync::Type::Fetch, ar.files[0].name, 0, 64);
    svc.handle(req, fn, out);
    TEST_ASSERT_EQUAL_INT(2, out.count);
    for (int i = 0; i < 2; ++i) {
        TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::Err, out.type(i));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)sync::Err::StorageFailed, out.err_code(i));
    }

    // storage claims ok but the card dies mid-request (list() -> -1)
    out.clear();
    svc.set_storage_ok(true);
    ar.fail = true;
    svc.handle(list_req, 2, out);
    TEST_ASSERT_EQUAL_INT(1, out.count);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)sync::Err::StorageFailed, out.err_code(0));
}

static void test_fetch_ranged_and_eof_semantics(void) {
    FakeArchive ar;
    fill_files(ar);
    SyncService svc(cfg(), ar);
    svc.set_storage_ok(true);
    CaptureSink out;
    uint8_t req[64];

    // full chunk
    size_t n = named_req(req, sync::Type::Fetch, ar.files[0].name, 0, 512);
    svc.handle(req, n, out);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::Data, out.type(0));
    TEST_ASSERT_EQUAL_UINT32(2 + 512, out.lens[0]);
    TEST_ASSERT_EQUAL_MEMORY(g_file_a, &out.frames[0][2], 512);

    // short read at EOF
    out.clear();
    n = named_req(req, sync::Type::Fetch, ar.files[0].name, 768, 512);
    svc.handle(req, n, out);
    TEST_ASSERT_EQUAL_UINT32(2 + 232, out.lens[0]);
    TEST_ASSERT_EQUAL_MEMORY(g_file_a + 768, &out.frames[0][2], 232);

    // exactly at end: empty DATA
    out.clear();
    n = named_req(req, sync::Type::Fetch, ar.files[0].name, 1000, 512);
    svc.handle(req, n, out);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::Data, out.type(0));
    TEST_ASSERT_EQUAL_UINT32(2, out.lens[0]);

    // past end: BadRange
    out.clear();
    n = named_req(req, sync::Type::Fetch, ar.files[0].name, 1001, 1);
    svc.handle(req, n, out);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::Err, out.type(0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)sync::Err::BadRange, out.err_code(0));

    // unknown name
    out.clear();
    n = named_req(req, sync::Type::Fetch, "nope.jsonl", 0, 16);
    svc.handle(req, n, out);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)sync::Err::UnknownName, out.err_code(0));

    // path traversal rejected by construction
    out.clear();
    n = named_req(req, sync::Type::Fetch, "../config.txt", 0, 16);
    svc.handle(req, n, out);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)sync::Err::BadRequest, out.err_code(0));
}

static void test_crc_chained_chunks_equal_whole_file(void) {
    FakeArchive ar;
    fill_files(ar);
    SyncService svc(cfg(), ar);
    svc.set_storage_ok(true);
    CaptureSink out;
    uint8_t req[64];

    // chunk 1: [0, 600) from the initial seed
    uint16_t seed = 0xFFFF;
    size_t n = named_req(req, sync::Type::Crc, ar.files[0].name, 0, 600, &seed);
    svc.handle(req, n, out);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::CrcOk, out.type(0));
    const uint16_t mid = (uint16_t)(out.frames[0][2] | (out.frames[0][3] << 8));

    // chunk 2: [600, 1000) seeded with chunk 1's result
    out.clear();
    n = named_req(req, sync::Type::Crc, ar.files[0].name, 600, 400, &mid);
    svc.handle(req, n, out);
    const uint16_t whole = (uint16_t)(out.frames[0][2] | (out.frames[0][3] << 8));
    TEST_ASSERT_EQUAL_HEX16(crc16(g_file_a, sizeof(g_file_a)), whole);

    // range overrunning the file: BadRange
    out.clear();
    seed = 0xFFFF;
    n = named_req(req, sync::Type::Crc, ar.files[0].name, 900, 200, &seed);
    svc.handle(req, n, out);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)sync::Err::BadRange, out.err_code(0));
}

static void test_delete_removes_closed_session(void) {
    FakeArchive ar;
    fill_files(ar);
    SyncService svc(cfg(), ar);
    svc.set_storage_ok(true);
    CaptureSink out;
    uint8_t req[64];

    // unknown name -> UnknownName, archive untouched
    size_t n = delete_req(req, "nope.jsonl");
    svc.handle(req, n, out);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::Err, out.type(0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)sync::Err::UnknownName, out.err_code(0));
    TEST_ASSERT_EQUAL_INT(2, ar.nfiles);

    // the actively-open session is refused, not deleted
    out.clear();
    svc.set_open_session(ar.files[0].name);
    n = delete_req(req, ar.files[0].name);
    svc.handle(req, n, out);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::Err, out.type(0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)sync::Err::SessionOpen, out.err_code(0));
    TEST_ASSERT_EQUAL_INT(2, ar.nfiles);
    svc.set_open_session(nullptr);

    // path traversal rejected by construction (shared parse_name guard)
    out.clear();
    n = delete_req(req, "../config.txt");
    svc.handle(req, n, out);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)sync::Err::BadRequest, out.err_code(0));

    // storage down -> explicit StorageFailed, never a hang (decision 5)
    out.clear();
    svc.set_storage_ok(false);
    n = delete_req(req, ar.files[0].name);
    svc.handle(req, n, out);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)sync::Err::StorageFailed, out.err_code(0));
    svc.set_storage_ok(true);

    // closed session: DeleteOk, and it's actually gone from LIST
    out.clear();
    const char* victim = ar.files[0].name;
    n = delete_req(req, victim);
    svc.handle(req, n, out);
    TEST_ASSERT_EQUAL_INT(1, out.count);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::DeleteOk, out.type(0));
    TEST_ASSERT_EQUAL_INT(1, ar.nfiles);

    const uint8_t list_req[2] = {sync::kChannelSync, (uint8_t)sync::Type::List};
    out.clear();
    svc.handle(list_req, 2, out);
    TEST_ASSERT_EQUAL_INT(2, out.count);  // one ITEM + LIST_END
    const uint8_t nlen = out.frames[0][2];
    TEST_ASSERT_EQUAL_UINT8(strlen("20260707T090000_22222222.jsonl"), nlen);
    TEST_ASSERT_EQUAL_MEMORY("20260707T090000_22222222.jsonl", &out.frames[0][3], nlen);
}

static void test_bye_ends_the_session_and_unknowns_are_explicit(void) {
    FakeArchive ar;
    SyncService svc(cfg(), ar);
    CaptureSink out;

    const uint8_t bye[2] = {sync::kChannelSync, (uint8_t)sync::Type::Bye};
    const SyncService::Result r = svc.handle(bye, 2, out);
    TEST_ASSERT_TRUE(r.session_over);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::ByeOk, out.type(0));

    // unknown request type -> explicit error
    out.clear();
    const uint8_t junk[2] = {sync::kChannelSync, 0x77};
    svc.handle(junk, 2, out);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)sync::Type::Err, out.type(0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)sync::Err::UnknownType, out.err_code(0));

    // foreign channel (reserved: future control plane) -> silently ignored
    out.clear();
    const uint8_t other[3] = {1, 0x01, 0x00};
    const SyncService::Result r2 = svc.handle(other, 3, out);
    TEST_ASSERT_EQUAL_INT(0, out.count);
    TEST_ASSERT_FALSE(r2.session_over);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_hello_ok_carries_versions_storage_and_time);
    RUN_TEST(test_hello_version_mismatch_refused);
    RUN_TEST(test_list_excludes_open_session);
    RUN_TEST(test_storage_failed_is_explicit_on_the_wire);
    RUN_TEST(test_fetch_ranged_and_eof_semantics);
    RUN_TEST(test_crc_chained_chunks_equal_whole_file);
    RUN_TEST(test_delete_removes_closed_session);
    RUN_TEST(test_bye_ends_the_session_and_unknowns_are_explicit);
    return UNITY_END();
}
