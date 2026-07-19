#include "edrum/sync_service.h"

#include <string.h>

#include "edrum/frame.h"

namespace edrum {

namespace {

uint16_t rd_u16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
void wr_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

}  // namespace

void SyncService::set_open_session(const char* name_or_null) {
    if (name_or_null == nullptr) {
        open_name_[0] = '\0';
        return;
    }
    strncpy(open_name_, name_or_null, sizeof(open_name_) - 1);
    open_name_[sizeof(open_name_) - 1] = '\0';
}

void SyncService::emit_err(FrameSink& out, sync::Err code) {
    const uint8_t payload[3] = {sync::kChannelSync, (uint8_t)sync::Type::Err, (uint8_t)code};
    out.emit(payload, sizeof(payload));
}

size_t SyncService::parse_name(const uint8_t* body, size_t len) {
    if (len < 1) return 0;
    const size_t nlen = body[0];
    if (nlen == 0 || nlen > hal::kSessionNameMax || len < 1 + nlen) return 0;
    memcpy(name_, body + 1, nlen);
    name_[nlen] = '\0';
    // reject path traversal by construction: names are bare filenames
    if (strchr(name_, '/') != nullptr || strchr(name_, '\\') != nullptr) return 0;
    return 1 + nlen;
}

SyncService::Result SyncService::handle(const uint8_t* payload, size_t len, FrameSink& out) {
    Result res;
    if (len < 2 || payload[0] != sync::kChannelSync) {
        return res;  // not ours (future channels): ignore, no response
    }
    const uint8_t type = payload[1];
    const uint8_t* body = payload + 2;
    const size_t body_len = len - 2;

    switch ((sync::Type)type) {
        case sync::Type::Hello:
            handle_hello(body, body_len, out, res);
            break;
        case sync::Type::List:
            handle_list(out);
            break;
        case sync::Type::Fetch:
            handle_fetch(body, body_len, out);
            break;
        case sync::Type::Crc:
            handle_crc(body, body_len, out);
            break;
        case sync::Type::Delete:
            handle_delete(body, body_len, out);
            break;
        case sync::Type::Bye: {
            const uint8_t bye[2] = {sync::kChannelSync, (uint8_t)sync::Type::ByeOk};
            out.emit(bye, sizeof(bye));
            res.session_over = true;
            break;
        }
        default:
            emit_err(out, sync::Err::UnknownType);
            break;
    }
    return res;
}

void SyncService::handle_hello(const uint8_t* body, size_t len, FrameSink& out, Result& res) {
    if (len < 2) {
        emit_err(out, sync::Err::BadRequest);
        return;
    }
    const uint8_t host_ver = body[0];
    if (host_ver != cfg_.proto_version) {
        // single-byte version = major-only, mirroring the schema policy:
        // refuse a mismatch explicitly rather than guessing.
        emit_err(out, sync::Err::UnsupportedVersion);
        return;
    }
    const size_t time_len = body[1];
    if (time_len > 0) {
        if (len < 2 + time_len || time_len > 32) {
            emit_err(out, sync::Err::BadRequest);
            return;
        }
        char iso[33];
        memcpy(iso, body + 2, time_len);
        iso[time_len] = '\0';
        DateTime dt;
        if (parse_iso(iso, dt)) {  // unparseable time: ignored, not fatal
            res.time_requested = true;
            res.host_time = dt;
        }
    }

    uint8_t reply[6 + 64];
    reply[0] = sync::kChannelSync;
    reply[1] = (uint8_t)sync::Type::HelloOk;
    reply[2] = cfg_.proto_version;
    reply[3] = cfg_.schema_version;
    reply[4] = storage_ok_ ? 1 : 0;
    size_t fw_len = strlen(cfg_.fw_build);
    if (fw_len > 64) fw_len = 64;
    reply[5] = (uint8_t)fw_len;
    memcpy(reply + 6, cfg_.fw_build, fw_len);
    out.emit(reply, 6 + fw_len);
}

void SyncService::handle_list(FrameSink& out) {
    if (!storage_ok_) {
        emit_err(out, sync::Err::StorageFailed);
        return;
    }
    const int n = archive_.list(entries_, sync::kListMax);
    if (n < 0) {
        emit_err(out, sync::Err::StorageFailed);
        return;
    }

    uint16_t count = 0;
    for (int i = 0; i < n; ++i) {
        if (open_name_[0] != '\0' && strcmp(entries_[i].name, open_name_) == 0) {
            continue;  // closed sessions only (spec §13)
        }
        const size_t nlen = strlen(entries_[i].name);
        uint8_t item[2 + 1 + hal::kSessionNameMax + 4];
        item[0] = sync::kChannelSync;
        item[1] = (uint8_t)sync::Type::Item;
        item[2] = (uint8_t)nlen;
        memcpy(item + 3, entries_[i].name, nlen);
        wr_u32(item + 3 + nlen, entries_[i].size);
        out.emit(item, 3 + nlen + 4);
        ++count;
    }

    uint8_t end[4];
    end[0] = sync::kChannelSync;
    end[1] = (uint8_t)sync::Type::ListEnd;
    wr_u16(end + 2, count);
    out.emit(end, sizeof(end));
}

void SyncService::handle_fetch(const uint8_t* body, size_t len, FrameSink& out) {
    if (!storage_ok_) {
        emit_err(out, sync::Err::StorageFailed);
        return;
    }
    const size_t used = parse_name(body, len);
    if (used == 0 || len != used + 6) {
        emit_err(out, sync::Err::BadRequest);
        return;
    }
    const uint32_t offset = rd_u32(body + used);
    uint16_t want = rd_u16(body + used + 4);
    if (want > sync::kFetchMax) want = sync::kFetchMax;

    const int64_t size = archive_.size_of(name_);
    if (size < 0) {
        emit_err(out, sync::Err::UnknownName);
        return;
    }
    if (offset > (uint64_t)size) {
        emit_err(out, sync::Err::BadRange);
        return;
    }

    int got = 0;
    if (offset < (uint64_t)size && want > 0) {
        got = archive_.read(name_, offset, io_, want);
        if (got < 0) {
            emit_err(out, sync::Err::StorageFailed);
            return;
        }
    }

    // DATA shorter than requested = EOF reached; empty DATA = exactly at end.
    uint8_t head[2] = {sync::kChannelSync, (uint8_t)sync::Type::Data};
    uint8_t reply[2 + sync::kFetchMax];
    memcpy(reply, head, 2);
    memcpy(reply + 2, io_, (size_t)got);
    out.emit(reply, 2 + (size_t)got);
}

void SyncService::handle_crc(const uint8_t* body, size_t len, FrameSink& out) {
    if (!storage_ok_) {
        emit_err(out, sync::Err::StorageFailed);
        return;
    }
    const size_t used = parse_name(body, len);
    if (used == 0 || len != used + 8) {
        emit_err(out, sync::Err::BadRequest);
        return;
    }
    const uint32_t offset = rd_u32(body + used);
    uint16_t span = rd_u16(body + used + 4);
    const uint16_t seed = rd_u16(body + used + 6);
    if (span > sync::kCrcChunkMax) span = sync::kCrcChunkMax;

    const int64_t size = archive_.size_of(name_);
    if (size < 0) {
        emit_err(out, sync::Err::UnknownName);
        return;
    }
    if ((uint64_t)offset + span > (uint64_t)size) {
        emit_err(out, sync::Err::BadRange);
        return;
    }

    uint16_t crc = seed;
    uint32_t done = 0;
    while (done < span) {
        uint32_t chunk = span - done;
        if (chunk > sync::kFetchMax) chunk = sync::kFetchMax;
        const int got = archive_.read(name_, offset + done, io_, chunk);
        if (got <= 0) {
            emit_err(out, sync::Err::StorageFailed);
            return;
        }
        crc = crc16(io_, (size_t)got, crc);
        done += (uint32_t)got;
    }

    uint8_t reply[4];
    reply[0] = sync::kChannelSync;
    reply[1] = (uint8_t)sync::Type::CrcOk;
    wr_u16(reply + 2, crc);
    out.emit(reply, sizeof(reply));
}

void SyncService::handle_delete(const uint8_t* body, size_t len, FrameSink& out) {
    if (!storage_ok_) {
        emit_err(out, sync::Err::StorageFailed);
        return;
    }
    const size_t used = parse_name(body, len);
    if (used == 0 || len != used) {
        emit_err(out, sync::Err::BadRequest);
        return;
    }
    if (open_name_[0] != '\0' && strcmp(name_, open_name_) == 0) {
        emit_err(out, sync::Err::SessionOpen);
        return;
    }
    if (archive_.size_of(name_) < 0) {
        emit_err(out, sync::Err::UnknownName);
        return;
    }
    if (!archive_.remove(name_)) {
        emit_err(out, sync::Err::StorageFailed);
        return;
    }
    const uint8_t ok[2] = {sync::kChannelSync, (uint8_t)sync::Type::DeleteOk};
    out.emit(ok, sizeof(ok));
}

}  // namespace edrum
