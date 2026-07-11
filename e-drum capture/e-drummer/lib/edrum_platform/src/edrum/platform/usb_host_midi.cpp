#include "edrum/platform/usb_host_midi.h"

#include <stdio.h>
#include <string.h>

namespace edrum {
namespace platform {

namespace {
// Small stack-format helper for status lines (keeps Serial out of this lib).
struct StatusBuf {
    char s[96];
};
}  // namespace

void UsbHostMidi::usb_lib_task(void* arg) {
    auto* self = static_cast<UsbHostMidi*>(arg);
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    const esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        self->sink_.on_status("usb: FATAL usb_host_install failed");
        vTaskDelete(nullptr);
        return;
    }
    self->host_installed_ = true;
    self->sink_.on_status("usb: host stack installed");

    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

bool UsbHostMidi::begin(int daemon_core, unsigned daemon_priority) {
    const BaseType_t ok =
        xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, this,
                                daemon_priority, nullptr, daemon_core);
    if (ok != pdPASS) return false;

    while (!host_installed_) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = this,
        },
    };
    const esp_err_t err = usb_host_client_register(&client_config, &client_);
    if (err != ESP_OK) {
        sink_.on_status("usb: FATAL client register failed");
        return false;
    }
    sink_.on_status("usb: client registered; waiting for device on OTG port");
    return true;
}

void UsbHostMidi::client_event_cb(const usb_host_client_event_msg_t* msg, void* arg) {
    auto* self = static_cast<UsbHostMidi*>(arg);
    switch (msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            self->handle_new_device(msg->new_dev.address);
            break;
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            if (msg->dev_gone.dev_hdl == self->device_) {
                self->handle_device_gone();
            }
            break;
        default:
            break;
    }
}

void UsbHostMidi::handle_new_device(uint8_t address) {
    pending_address_ = address;
    open_pending_ = true;
}

void UsbHostMidi::handle_device_gone() {
    close_pending_ = true;
}

void UsbHostMidi::poll(uint32_t timeout_ms) {
    if (client_ == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return;
    }
    usb_host_client_handle_events(client_, pdMS_TO_TICKS(timeout_ms));

    // Deferred work, close first (fast unplug/replug leaves both pending).
    if (close_pending_) {
        close_pending_ = false;
        teardown_device();
    }
    if (open_pending_) {
        open_pending_ = false;
        if (device_ == nullptr) {
            open_and_claim();
        }
    }
}

void UsbHostMidi::open_and_claim() {
    esp_err_t err = usb_host_device_open(client_, pending_address_, &device_);
    if (err != ESP_OK) {
        device_ = nullptr;
        sink_.on_status("usb: device open FAILED");
        return;
    }

    const usb_config_desc_t* cfg = nullptr;
    err = usb_host_get_active_config_descriptor(device_, &cfg);
    if (err != ESP_OK || cfg == nullptr) {
        sink_.on_status("usb: config descriptor read FAILED");
        teardown_device();
        return;
    }

    iface_ = find_midi_streaming(reinterpret_cast<const uint8_t*>(cfg), cfg->wTotalLength);
    if (!iface_.found || !iface_.has_in_ep) {
        sink_.on_status("usb: no MIDI Streaming interface with IN endpoint — not a MIDI source");
        teardown_device();
        return;
    }

    err = usb_host_interface_claim(client_, device_, iface_.interface_number,
                                   iface_.alt_setting);
    if (err != ESP_OK) {
        sink_.on_status("usb: interface claim FAILED");
        teardown_device();
        return;
    }
    claimed_ = true;

    {
        StatusBuf b;
        snprintf(b.s, sizeof(b.s),
                 "usb: claimed MIDI iface %u, IN ep 0x%02X (%s, MPS %u)",
                 (unsigned)iface_.interface_number, (unsigned)iface_.in_ep_addr,
                 iface_.in_ep_attr == 0x02 ? "bulk" : "interrupt",
                 (unsigned)iface_.in_ep_mps);
        sink_.on_status(b.s);
    }

    // IN transfers: num_bytes must be an integer multiple of MPS.
    for (int i = 0; i < kNumTransfers; ++i) {
        usb_transfer_t* xfer = nullptr;
        if (usb_host_transfer_alloc(iface_.in_ep_mps, 0, &xfer) != ESP_OK) {
            sink_.on_status("usb: transfer alloc FAILED");
            teardown_device();
            return;
        }
        xfer->device_handle = device_;
        xfer->bEndpointAddress = iface_.in_ep_addr;
        xfer->callback = transfer_cb;
        xfer->context = this;
        xfer->num_bytes = iface_.in_ep_mps;
        transfers_[i] = xfer;
    }
    for (int i = 0; i < kNumTransfers; ++i) {
        if (usb_host_transfer_submit(transfers_[i]) != ESP_OK) {
            sink_.on_status("usb: transfer submit FAILED");
            teardown_device();
            return;
        }
    }

    streaming_ = true;
    counters_.cb_gap.begin_stream();  // don't measure a gap across the attach
    sink_.on_status("usb: MIDI stream running");
    sink_.on_device(true);
}

void UsbHostMidi::teardown_device() {
    streaming_ = false;
    if (device_ != nullptr) {
        if (claimed_) {
            usb_host_interface_release(client_, device_, iface_.interface_number);
            claimed_ = false;
        }
        for (int i = 0; i < kNumTransfers; ++i) {
            if (transfers_[i] != nullptr) {
                usb_host_transfer_free(transfers_[i]);
                transfers_[i] = nullptr;
            }
        }
        usb_host_device_close(client_, device_);
        device_ = nullptr;
        sink_.on_status("usb: device closed — ready for replug");
        sink_.on_device(false);
    }
    iface_ = MidiStreamingInfo{};
}

void UsbHostMidi::transfer_cb(usb_transfer_t* transfer) {
    static_cast<UsbHostMidi*>(transfer->context)->on_transfer_done(transfer);
}

void UsbHostMidi::on_transfer_done(usb_transfer_t* transfer) {
    // Rule 2: the stamp is taken before ANY other work on the arrival path.
    const uint64_t t_us = clock_.now_us();

    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        counters_.usb_transfers++;

        // Experiment 1 instrumentation: inter-arrival spacing between
        // completions (idle vs burst vs distribution — see cb_gap.h).
        counters_.cb_gap.record(t_us);

        current_t_us_ = t_us;
        parser_.feed(transfer->data_buffer, (size_t)transfer->actual_num_bytes);

        if (streaming_ && usb_host_transfer_submit(transfer) != ESP_OK) {
            counters_.usb_transfer_errors++;
        }
        return;
    }

    if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
        transfer->status == USB_TRANSFER_STATUS_CANCELED) {
        return;  // teardown owns cleanup
    }

    counters_.usb_transfer_errors++;
    if (streaming_ && usb_host_transfer_submit(transfer) != ESP_OK) {
        counters_.usb_transfer_errors++;
    }
}

void UsbHostMidi::on_midi(const MidiMsg& msg) {
    counters_.midi_msgs++;
    sink_.on_midi(current_t_us_, msg);
}

void UsbHostMidi::on_sysex(const uint8_t* data, uint8_t len, bool truncated) {
    counters_.sysex_msgs++;
    if (truncated) counters_.sysex_truncated++;
    sink_.on_sysex(current_t_us_, data, len, truncated);
}

}  // namespace platform
}  // namespace edrum
