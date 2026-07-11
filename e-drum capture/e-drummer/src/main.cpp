// e-drummer capture firmware — composition root.
//
// This file is deliberately thin (the brain's cli/ discipline): construct
// every object once, wire ports to adapters, start tasks, and get out of
// the way. No capture, storage, click, or session logic lives here.
//
//   capture task (core 0)  = USB host poll -> classify -> session FSM -> ring
//   click task   (core 0)  = schedule -> I2S DAC (device is tempo authority)
//   storage task (core 1)  = ring -> LogWriter -> SdFat (append-only log)
//   loop()       (core 1)  = serial console (Communications)
//
// The enumeration probe this firmware grew from is preserved at
// tools/probe/usb_enumeration_probe.cpp (roadmap Experiment 2 artifact).
#include "app.h"

#include <SPI.h>

App app;

namespace {

void load_config() {
    if (!app.storage_ok) return;
    static char buf[2048];
    const int n = app.storage.read_file(appcfg::kConfigPath, buf, sizeof(buf) - 1);
    if (n <= 0) {
        Serial.printf("[cfg] no %s — using defaults\n", appcfg::kConfigPath);
        return;
    }
    const edrum::ConfigParseStats st = edrum::parse_config(buf, (size_t)n, app.cfg);
    Serial.printf("[cfg] %s: %u applied, %u unknown, %u invalid\n", appcfg::kConfigPath,
                  st.applied, st.unknown, st.invalid);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(250);
    Serial.println();
    Serial.println("=================================================================");
    Serial.println(" e-drummer capture firmware");
    Serial.printf(" build %s %s   schema v%d\n", __DATE__, __TIME__, edrum::kSchemaVersion);
    Serial.println(" monitor on UART port; kit on native USB-OTG port (spec 8.3)");
    Serial.println(" type `help` for the console");
    Serial.println("=================================================================");

    // storage first: config lives on the card
    SPI.begin(appcfg::kSdSck, appcfg::kSdMiso, appcfg::kSdMosi, appcfg::kSdCs);
    app.storage_ok =
        app.storage.begin(appcfg::kSdCs, appcfg::kSdMaxHz, appcfg::kSessionDir);
    Serial.printf("[sd] %s\n", app.storage_ok
                                   ? "mounted; sessions -> /sessions"
                                   : "MOUNT FAILED — capture will run, nothing persists");
    load_config();

    app.wall.begin(app.cfg.tz_offset_min);
    if (!app.wall.time_was_set()) {
        Serial.println("[time] UNSET — session order is kept, dates need `settime`");
    }

    // meta statics from config (kit_profile_id stays a re-pointable hint)
    strncpy(app.meta.kit_profile_id, app.cfg.kit_profile_id,
            sizeof(app.meta.kit_profile_id) - 1);
    strncpy(app.meta.user_id, app.cfg.user_id, sizeof(app.meta.user_id) - 1);
    app.meta.has_calibration = app.cfg.has_calibration;
    app.meta.calibration_offset_ms = app.cfg.calibration_offset_ms;

    // capture core objects (single construction, wired by reference)
    struct RingSink : edrum::RecordSink {
        bool push(const edrum::CaptureRecord& rec) override { return app.ring.push(rec); }
    };
    static RingSink ring_sink;

    edrum::SessionController::Cfg fsm_cfg;
    fsm_cfg.pause_after_ms = app.cfg.pause_after_ms;
    fsm_cfg.idle_end_ms = app.cfg.idle_end_ms;
    fsm_cfg.tz_offset_min = app.cfg.tz_offset_min;
    static edrum::SessionController fsm(fsm_cfg, ring_sink, app.wall, app.random,
                                        app.counters);
    app.fsm = &fsm;

    edrum::GestureDetector::Cfg gcfg;
    gcfg.enabled = app.cfg.gesture_enable;
    gcfg.note_a = app.cfg.gesture_note_a;
    gcfg.note_b = app.cfg.gesture_note_b;
    static edrum::GestureDetector gestures(gcfg);
    app.gestures = &gestures;

    // (the USB host adapter is constructed by the capture task, which owns
    // its callback sink — see capture_task.cpp)

    edrum::LogWriter::Cfg wcfg{appcfg::kSessionDir, appcfg::kSyncEveryBytes,
                               appcfg::kSyncEveryMs};
    static edrum::LogWriter writer(wcfg, app.storage, app.clock, app.meta, app.counters);
    app.writer = &writer;

    static edrum::ClickRenderer click_render(app.click_sched, app.counters);
    app.click_render = &click_render;
    static edrum::platform::I2sClickOut audio(appcfg::kI2sBclk, appcfg::kI2sLrck,
                                              appcfg::kI2sDout);
    app.audio = &audio;
    app.audio_ok = audio.begin(appcfg::kSampleRate);
    if (app.audio_ok) click_render.begin(appcfg::kSampleRate);
    Serial.printf("[i2s] %s\n", app.audio_ok ? "DAC ready (click -> TD-02 MIX IN)"
                                             : "INIT FAILED — click disabled");

    app.control_queue = xQueueCreate(appcfg::kControlQueueLen, sizeof(ControlMsg));

    xTaskCreatePinnedToCore(storage_task, "storage", 6144, nullptr,
                            appcfg::kStoragePrio, nullptr, appcfg::kStorageCore);
    xTaskCreatePinnedToCore(click_task, "click", 4096, nullptr, appcfg::kClickPrio,
                            nullptr, appcfg::kClickCore);
    xTaskCreatePinnedToCore(capture_task, "capture", 8192, nullptr,
                            appcfg::kCapturePrio, nullptr, appcfg::kCaptureCore);
    // capture_task constructs/starts the USB host machinery itself
}

void loop() {
    console_poll();
    delay(10);
}
