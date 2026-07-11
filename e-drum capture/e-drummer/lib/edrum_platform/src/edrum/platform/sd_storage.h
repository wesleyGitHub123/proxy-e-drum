// IStorage adapter over SdFat (SPI microSD module, capture spec §6/8.4).
// Single-open-file model matching the port contract. create() is exclusive
// (O_EXCL): session files are never rewritten (brain spec §3).
//
// SdFat (greiman/SdFat 2.x) chosen per the approved architecture plan; the
// deciding features over the bundled SD lib are explicit sync() granularity
// and dedicated-SPI performance — both Experiment 3 levers. The choice is
// contained here: swapping libraries touches this file only.
#pragma once

#include <SdFat.h>
#include <SPI.h>

#include "edrum/hal/ports.h"

namespace edrum {
namespace platform {

class SdStorage : public hal::IStorage {
public:
    // Mounts the card and ensures `dir` exists. SPI must already be begun
    // with the board pin map (composition root's job).
    bool begin(uint8_t cs_pin, uint32_t max_hz, const char* dir) {
        if (!sd_.begin(SdSpiConfig(cs_pin, SHARED_SPI, max_hz))) {
            return false;
        }
        if (!sd_.exists(dir) && !sd_.mkdir(dir)) {
            return false;
        }
        return true;
    }

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

private:
    SdFat sd_;
    FsFile file_;
};

}  // namespace platform
}  // namespace edrum
