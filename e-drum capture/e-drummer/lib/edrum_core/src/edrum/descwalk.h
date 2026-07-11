// USB configuration-descriptor walk — pure TLV parsing over the raw
// descriptor blob. Finds the MIDI Streaming interface (class 0x01 subclass
// 0x03) and its IN/OUT endpoints. This is the probe's descriptor knowledge
// (tools/probe) transmuted from reporting into configuration: the platform
// USB host adapter uses the result to claim the interface and submit
// transfers. Natively testable against scripted descriptor blobs.
#pragma once

#include <stdint.h>

namespace edrum {

struct MidiStreamingInfo {
    bool found = false;
    uint8_t interface_number = 0;
    uint8_t alt_setting = 0;
    bool has_in_ep = false;
    uint8_t in_ep_addr = 0;    // includes direction bit (0x8x)
    uint16_t in_ep_mps = 0;    // max packet size
    uint8_t in_ep_attr = 0;    // bmAttributes (0x02 bulk / 0x03 interrupt)
    bool has_out_ep = false;
    uint8_t out_ep_addr = 0;
    uint16_t out_ep_mps = 0;
};

// `cfg` points at the config descriptor header; `total_len` = wTotalLength.
// The first MIDI Streaming interface (alt 0 preferred) wins.
MidiStreamingInfo find_midi_streaming(const uint8_t* cfg, uint16_t total_len);

}  // namespace edrum
