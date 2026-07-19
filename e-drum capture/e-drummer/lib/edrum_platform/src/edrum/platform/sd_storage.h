// IStorage + ISessionArchive adapter over SdFat (SPI microSD module,
// capture spec §6/8.4). One class implements both ports on purpose: there
// is exactly one card and one owner (the storage task), so keeping the
// write side and the sync protocol's read side in a single adapter is what
// preserves the single-SD-owner discipline by construction. The ports stay
// narrow and separate — the write side keeps its single-open-file
// append-only model; the archive side never touches the open file (the
// sync service excludes it via set_open_session).
//
// create() is exclusive (O_EXCL): session files are never rewritten (brain
// spec §3).
//
// SdFat (greiman/SdFat 2.x) chosen per the approved architecture plan; the
// deciding features over the bundled SD lib are explicit sync() granularity
// and dedicated-SPI performance — both Experiment 3 levers. The choice is
// contained here: swapping libraries touches this file only.
#pragma once

#include <stdio.h>
#include <string.h>

#include <SdFat.h>
#include <SPI.h>

#include "edrum/hal/ports.h"

namespace edrum {
namespace platform {

class SdStorage : public hal::IStorage, public hal::ISessionArchive {
public:
    // Mounts the card and ensures `dir` exists. SPI must already be begun
    // with the board pin map (composition root's job).
    bool begin(uint8_t cs_pin, uint32_t max_hz, const char* dir) {
        strncpy(dir_, dir, sizeof(dir_) - 1);
        dir_[sizeof(dir_) - 1] = '\0';
        if (!sd_.begin(SdSpiConfig(cs_pin, SHARED_SPI, max_hz))) {
            return false;
        }
        if (!sd_.exists(dir) && !sd_.mkdir(dir)) {
            return false;
        }
        return true;
    }

    // --- hal::IStorage (write side; single open file) ----------------------

    bool exists(const char* path) override { return sd_.exists(path); }

    int read_file(const char* path, char* buf, size_t cap) override {
        FsFile f = sd_.open(path, O_RDONLY);
        if (!f) return -1;
        const int n = f.read(buf, cap);
        f.close();
        return n;
    }

    bool create(const char* path) override {
        if (file_) file_.close();
        file_ = sd_.open(path, O_WRONLY | O_CREAT | O_EXCL);
        return (bool)file_;
    }

    bool append(const uint8_t* data, size_t len) override {
        if (!file_) return false;
        return file_.write(data, len) == len;
    }

    bool sync() override {
        if (!file_) return false;
        return file_.sync();
    }

    void close() override {
        if (file_) file_.close();
    }

    // --- hal::ISessionArchive (read side; sync protocol, spec §13) ---------
    // Called from the storage task only — same task as the writer, so SD
    // access stays single-threaded with no locking.

    int list(hal::SessionInfo* out, size_t cap) override {
        FsFile dir = sd_.open(dir_, O_RDONLY);
        if (!dir || !dir.isDir()) return -1;
        int n = 0;
        FsFile f;
        while (n < (int)cap && f.openNext(&dir, O_RDONLY)) {
            char name[hal::kSessionNameMax + 1] = {0};
            f.getName(name, sizeof(name));
            const bool is_dir = f.isDir();
            const uint32_t size = (uint32_t)f.fileSize();
            f.close();
            if (is_dir) continue;
            const size_t len = strlen(name);
            if (len < 7 || strcmp(name + len - 6, ".jsonl") != 0) continue;
            memcpy(out[n].name, name, len + 1);
            out[n].size = size;
            ++n;
        }
        dir.close();
        return n;
    }

    int64_t size_of(const char* name) override {
        char path[96];
        if (!join(path, sizeof(path), name)) return -1;
        FsFile f = sd_.open(path, O_RDONLY);
        if (!f) return -1;
        const int64_t sz = (int64_t)f.fileSize();
        f.close();
        return sz;
    }

    int read(const char* name, uint32_t offset, uint8_t* buf, uint32_t len) override {
        char path[96];
        if (!join(path, sizeof(path), name)) return -1;
        FsFile f = sd_.open(path, O_RDONLY);
        if (!f) return -1;
        if (!f.seekSet(offset)) {
            f.close();
            return -1;
        }
        const int n = f.read(buf, len);
        f.close();
        return n;
    }

    bool remove(const char* name) override {
        char path[96];
        if (!join(path, sizeof(path), name)) return false;
        return sd_.remove(path);
    }

private:
    bool join(char* out, size_t cap, const char* name) {
        const int n = snprintf(out, cap, "%s/%s", dir_, name);
        return n > 0 && (size_t)n < cap;
    }

    SdFat sd_;
    FsFile file_;
    char dir_[32] = {0};
};

}  // namespace platform
}  // namespace edrum
