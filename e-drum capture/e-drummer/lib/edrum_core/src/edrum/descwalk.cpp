#include "edrum/descwalk.h"

namespace edrum {

namespace {
constexpr uint8_t kDescInterface = 0x04;
constexpr uint8_t kDescEndpoint = 0x05;
}  // namespace

MidiStreamingInfo find_midi_streaming(const uint8_t* cfg, uint16_t total_len) {
    MidiStreamingInfo info;
    if (cfg == nullptr || total_len < 4) return info;

    bool in_target_iface = false;
    uint16_t offset = cfg[0];  // skip the config descriptor header itself

    while (offset + 1 < total_len) {
        const uint8_t length = cfg[offset];
        const uint8_t type = cfg[offset + 1];
        if (length == 0 || (uint32_t)offset + length > total_len) break;

        if (type == kDescInterface && length >= 9) {
            const uint8_t iface_num = cfg[offset + 2];
            const uint8_t alt = cfg[offset + 3];
            const uint8_t cls = cfg[offset + 5];
            const uint8_t sub = cfg[offset + 6];
            const bool is_midi = (cls == 0x01 && sub == 0x03);

            if (info.found && iface_num != info.interface_number) {
                break;  // walked past the matched interface's descriptors
            }
            if (is_midi && !info.found) {
                info.found = true;
                info.interface_number = iface_num;
                info.alt_setting = alt;
                in_target_iface = true;
            } else if (info.found && iface_num == info.interface_number && alt != info.alt_setting) {
                in_target_iface = false;  // other alt settings: skip endpoints
            } else if (!is_midi) {
                in_target_iface = false;
            }
        } else if (type == kDescEndpoint && length >= 7 && in_target_iface) {
            const uint8_t addr = cfg[offset + 2];
            const uint8_t attr = cfg[offset + 3];
            const uint16_t mps =
                (uint16_t)(cfg[offset + 4] | ((uint16_t)cfg[offset + 5] << 8)) & 0x07FF;
            const uint8_t xfer = attr & 0x03;
            if (xfer == 0x02 || xfer == 0x03) {  // bulk or interrupt only
                if ((addr & 0x80) != 0) {
                    if (!info.has_in_ep) {
                        info.has_in_ep = true;
                        info.in_ep_addr = addr;
                        info.in_ep_mps = mps;
                        info.in_ep_attr = xfer;
                    }
                } else if (!info.has_out_ep) {
                    info.has_out_ep = true;
                    info.out_ep_addr = addr;
                    info.out_ep_mps = mps;
                }
            }
        }
        offset = (uint16_t)(offset + length);
    }
    return info;
}

}  // namespace edrum
