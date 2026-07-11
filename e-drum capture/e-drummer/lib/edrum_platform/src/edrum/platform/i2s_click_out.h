// IAudioOut adapter: legacy ESP-IDF I2S driver (Arduino core 2.x / IDF 4.4)
// driving an external I2S DAC (PCM5102 / MAX98357A — capture spec §4: the
// S3 has no analog DAC; the click leaves over I2S into the TD-02 MIX IN).
//
// Mono frames in, duplicated to a stereo standard-I2S stream. write()
// blocks on DMA backpressure — that blocking IS the click task's pacing.
// DMA depth (4 x 64 frames ≈ 5.3 ms at 48 kHz) is constant latency and
// lands in calibration_offset_ms (capture spec §4).
#pragma once

extern "C" {
#include "driver/i2s.h"
}

#include "edrum/hal/ports.h"

namespace edrum {
namespace platform {

class I2sClickOut : public hal::IAudioOut {
public:
    I2sClickOut(int bclk_pin, int lrck_pin, int dout_pin, i2s_port_t port = I2S_NUM_0)
        : bclk_(bclk_pin), lrck_(lrck_pin), dout_(dout_pin), port_(port) {}

    bool begin(uint32_t sample_rate) override {
        i2s_config_t cfg = {};
        cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
        cfg.sample_rate = sample_rate;
        cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
        cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
        cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
        cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
        cfg.dma_buf_count = 4;
        cfg.dma_buf_len = 64;  // frames per DMA buffer
        cfg.use_apll = false;
        cfg.tx_desc_auto_clear = true;  // underrun -> silence, never stale data

        if (i2s_driver_install(port_, &cfg, 0, nullptr) != ESP_OK) return false;

        i2s_pin_config_t pins = {};
        pins.mck_io_num = I2S_PIN_NO_CHANGE;
        pins.bck_io_num = bclk_;
        pins.ws_io_num = lrck_;
        pins.data_out_num = dout_;
        pins.data_in_num = I2S_PIN_NO_CHANGE;
        if (i2s_set_pin(port_, &pins) != ESP_OK) return false;
        return true;
    }

    size_t write(const int16_t* frames, size_t nframes) override {
        size_t written_frames = 0;
        while (written_frames < nframes) {
            size_t chunk = nframes - written_frames;
            if (chunk > kChunk) chunk = kChunk;
            for (size_t i = 0; i < chunk; ++i) {
                stereo_[i * 2] = frames[written_frames + i];
                stereo_[i * 2 + 1] = frames[written_frames + i];
            }
            size_t bytes_written = 0;
            if (i2s_write(port_, stereo_, chunk * 2 * sizeof(int16_t), &bytes_written,
                          portMAX_DELAY) != ESP_OK) {
                break;
            }
            written_frames += bytes_written / (2 * sizeof(int16_t));
        }
        return written_frames;
    }

private:
    static constexpr size_t kChunk = 64;
    int bclk_, lrck_, dout_;
    i2s_port_t port_;
    int16_t stereo_[kChunk * 2];
};

}  // namespace platform
}  // namespace edrum
