// The session-archive sync protocol, device side (capture spec §13) — a
// pure request→response state machine: CRC-verified payloads in, response
// payloads out through a FrameSink. No I/O, no OS, no transport knowledge
// (the same discipline as ControlDispatcher): the storage task pumps bytes
// through frame.h and this class, and any future transport (BLE, Wi-Fi,
// TCP) reuses both untouched.
//
// The logical contract this implements: the device is the writer-of-record;
// the brain replicates its archive byte-exactly. LIST/FETCH/CRC move the
// canonical session-log bytes unchanged — the brain must never be able to
// tell a synced file from a card-reader copy. Closed sessions only (the
// open session is excluded via set_open_session, and DELETE explicitly
// refuses it too — SessionOpen). No rename, no write: DELETE is the one
// narrow exception to read-only, and only because it is still a purely
// host-requested act — the device never deletes on its own (no auto-prune,
// no free-space eviction). Retention stays a decision the host makes, never
// something the protocol does for you.
//
// Fail-soft (storage policy decision 5): when storage is down (mount
// failure, writer latched failed), LIST/FETCH/CRC answer ERR StorageFailed
// on the wire — the protocol degrades explicitly, never hangs; the device
// keeps running as a standalone metronome/capture unit.
//
// Versioning mirrors the schema reader policy (brain spec §3): HELLO
// carries the protocol version; a mismatch is refused with an explicit
// error, never guessed around.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edrum/hal/ports.h"
#include "edrum/timefmt.h"

namespace edrum {

namespace sync {

constexpr uint8_t kProtoVersion = 1;
constexpr uint8_t kChannelSync = 0;  // channel 1 reserved: future control plane

enum class Type : uint8_t {
    // host -> device
    Hello = 0x01,
    List = 0x02,
    Fetch = 0x03,
    Crc = 0x04,
    Bye = 0x05,
    Delete = 0x06,
    // device -> host (request | 0x80)
    HelloOk = 0x81,
    Item = 0x82,
    ListEnd = 0x83,
    Data = 0x84,
    CrcOk = 0x85,
    ByeOk = 0x86,
    DeleteOk = 0x87,
    Err = 0xFF,
};

enum class Err : uint8_t {
    BadRequest = 1,
    UnknownType = 2,
    UnknownName = 3,
    BadRange = 4,
    StorageFailed = 5,  // decision 5: explicit, never a hang
    UnsupportedVersion = 6,
    SessionOpen = 7,    // DELETE refuses the actively-written session
};

// Work bounds per request — these cap how long one storage-task iteration
// can hold the SD, so a transfer can never starve the ring drain.
constexpr uint16_t kFetchMax = 512;
constexpr uint16_t kCrcChunkMax = 4096;
constexpr size_t kListMax = 64;

}  // namespace sync

// Where response payloads go. The composition root's sink frame-encodes and
// writes to the IByteLink; tests capture and decode.
struct FrameSink {
    virtual void emit(const uint8_t* payload, size_t len) = 0;
    virtual ~FrameSink() = default;
};

class SyncService {
public:
    struct Cfg {
        uint8_t proto_version = sync::kProtoVersion;
        uint8_t schema_version = 0;   // kSchemaVersion at composition
        const char* fw_build = "";    // shown to the host in HELLO_OK
    };

    SyncService(const Cfg& cfg, hal::ISessionArchive& archive)
        : cfg_(cfg), archive_(archive) {}

    // Composition-root state, refreshed by the storage task before each pump.
    void set_storage_ok(bool ok) { storage_ok_ = ok; }
    void set_open_session(const char* name_or_null);

    // Side effects the CALLER applies (the service stays pure): HELLO may
    // carry host wall time (-> IWallClock.set_time at the composition root);
    // BYE ends the sync session (-> console restored).
    struct Result {
        bool time_requested = false;
        DateTime host_time;
        bool session_over = false;
    };

    // Feed one CRC-verified payload (channel byte included). Responses are
    // emitted through `out`; payloads not addressed to the sync channel are
    // ignored (reserved for future channels).
    Result handle(const uint8_t* payload, size_t len, FrameSink& out);

private:
    void emit_err(FrameSink& out, sync::Err code);
    void handle_hello(const uint8_t* body, size_t len, FrameSink& out, Result& res);
    void handle_list(FrameSink& out);
    void handle_fetch(const uint8_t* body, size_t len, FrameSink& out);
    void handle_crc(const uint8_t* body, size_t len, FrameSink& out);
    void handle_delete(const uint8_t* body, size_t len, FrameSink& out);

    // Parse "name_len u8 | name | ..." into name_; returns bytes consumed
    // or 0 on malformed input.
    size_t parse_name(const uint8_t* body, size_t len);

    Cfg cfg_;
    hal::ISessionArchive& archive_;
    bool storage_ok_ = false;
    char open_name_[hal::kSessionNameMax + 1] = {0};

    char name_[hal::kSessionNameMax + 1] = {0};
    hal::SessionInfo entries_[sync::kListMax];
    uint8_t io_[sync::kFetchMax];
};

}  // namespace edrum
