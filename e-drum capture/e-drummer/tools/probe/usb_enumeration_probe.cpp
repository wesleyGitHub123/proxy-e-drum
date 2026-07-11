#include <Arduino.h>
#include <stdarg.h>

extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
}

// ---------------------------------------------------------------------------
// USB host enumeration probe — roadmap Experiment 2 (VBUS + enumeration).
// Proves the S3's native host path can attach, address, and read a device's
// full descriptor set. No transfers beyond EP0; no capture logic.
//
// Two tasks, per the ESP-IDF host model:
//   usb_lib_task    — daemon; drives the hub/enumeration state machine
//   usb_client_task — client; receives NEW_DEV/DEV_GONE, opens the device
//
// Serial monitor lives on the UART/COM port (the OTG port is consumed by
// host mode — capture spec §8.3).
// ---------------------------------------------------------------------------

static usb_host_client_handle_t s_client_handle = nullptr;
static usb_device_handle_t s_device_handle = nullptr;
static volatile bool s_host_installed = false;
static volatile bool s_open_pending = false;
static volatile bool s_close_pending = false;
static volatile uint8_t s_open_address = 0;

// Timestamped stage line: "[+  12.345] ..." — the experiment doc requires a
// timestamp on every stage so runs can be correlated with meter readings.
static void log_line(const char *fmt, ...) {
    char body[224];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    const uint32_t ms = millis();
    Serial.printf("[+%5lu.%03lu] %s\n",
                  (unsigned long)(ms / 1000), (unsigned long)(ms % 1000), body);
}

static const char *speed_name(usb_speed_t speed) {
    switch (speed) {
    case USB_SPEED_LOW:  return "Low Speed (1.5 Mbps)";
    case USB_SPEED_FULL: return "Full Speed (12 Mbps)";
    default:             return "Unknown speed";
    }
}

static const char *class_name(uint8_t cls, uint8_t subcls) {
    switch (cls) {
    case 0x00: return "defined per interface";
    case 0x01:
        if (subcls == 0x01) return "Audio Control";
        if (subcls == 0x02) return "Audio Streaming";
        if (subcls == 0x03) return "MIDI Streaming";
        return "Audio";
    case 0x02: return "CDC Control";
    case 0x03: return "HID";
    case 0x08: return "Mass Storage";
    case 0x09: return "Hub";
    case 0x0A: return "CDC Data";
    case 0xEF: return "Miscellaneous (IAD)";
    case 0xFF: return "Vendor Specific";
    default:   return "Unknown";
    }
}

// USB string descriptors are UTF-16LE; flatten to printable ASCII.
static void str_desc_to_ascii(const usb_str_desc_t *desc, char *out, size_t out_len) {
    if (desc == nullptr || desc->bLength < 2) {
        strlcpy(out, "(none)", out_len);
        return;
    }
    const size_t chars = (desc->bLength - 2) / 2;
    size_t j = 0;
    for (size_t i = 0; i < chars && j + 1 < out_len; ++i) {
        const uint16_t c = desc->wData[i];
        out[j++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out[j] = '\0';
}

struct InterfaceTally {
    uint16_t total = 0;
    uint16_t audio_control = 0;
    uint16_t audio_streaming = 0;
    uint16_t midi_streaming = 0;
    uint16_t hid = 0;
    uint16_t vendor = 0;
    uint16_t endpoints = 0;
};

static void print_device_report(usb_device_handle_t dev) {
    usb_device_info_t info;
    esp_err_t err = usb_host_device_info(dev, &info);
    if (err != ESP_OK) {
        log_line("[usb] ERROR: device info read failed: %s", esp_err_to_name(err));
        return;
    }

    const usb_device_desc_t *dd = nullptr;
    err = usb_host_get_device_descriptor(dev, &dd);
    if (err != ESP_OK) {
        log_line("[usb] ERROR: device descriptor read FAILED: %s", esp_err_to_name(err));
        return;
    }
    log_line("[usb] device descriptor read OK");

    const usb_config_desc_t *cfg = nullptr;
    err = usb_host_get_active_config_descriptor(dev, &cfg);
    if (err != ESP_OK) {
        log_line("[usb] ERROR: config descriptor read FAILED: %s", esp_err_to_name(err));
        return;
    }
    log_line("[usb] config descriptor read OK (wTotalLength=%u bytes)", cfg->wTotalLength);

    char manufacturer[64], product[64], serial[64];
    str_desc_to_ascii(info.str_desc_manufacturer, manufacturer, sizeof(manufacturer));
    str_desc_to_ascii(info.str_desc_product, product, sizeof(product));
    str_desc_to_ascii(info.str_desc_serial_num, serial, sizeof(serial));

    Serial.println();
    Serial.println("======================== USB DEVICE REPORT ========================");
    Serial.println("Bus");
    Serial.printf(" \xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 Device address %u \xE2\x80\x94 %s\n",
                  info.dev_addr, speed_name(info.speed));
    Serial.printf("      \xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 VID:PID         0x%04X:0x%04X\n",
                  dd->idVendor, dd->idProduct);
    Serial.printf("      \xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 USB version     %x.%02x   (device release %x.%02x)\n",
                  dd->bcdUSB >> 8, dd->bcdUSB & 0xFF, dd->bcdDevice >> 8, dd->bcdDevice & 0xFF);
    Serial.printf("      \xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 Device class    0x%02X/0x%02X/0x%02X  [%s]\n",
                  dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol,
                  class_name(dd->bDeviceClass, dd->bDeviceSubClass));
    Serial.printf("      \xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 EP0 max packet  %u bytes   Configurations: %u\n",
                  dd->bMaxPacketSize0, dd->bNumConfigurations);
    Serial.printf("      \xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 Manufacturer    %s\n", manufacturer);
    Serial.printf("      \xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 Product         %s\n", product);
    Serial.printf("      \xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 Serial          %s\n", serial);

    const bool self_powered = (cfg->bmAttributes & 0x40) != 0;
    Serial.printf("      \xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 Configuration %u \xE2\x80\x94 %s, max %u mA, %u interface(s)\n",
                  cfg->bConfigurationValue, self_powered ? "self-powered" : "bus-powered",
                  cfg->bMaxPower * 2, cfg->bNumInterfaces);

    // Walk the raw descriptor blob that follows the config header.
    InterfaceTally tally;
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(cfg);
    const uint16_t total_length = cfg->wTotalLength;

    for (uint16_t offset = cfg->bLength; offset + 1 < total_length;) {
        const uint8_t length = raw[offset];
        const uint8_t type = raw[offset + 1];
        if (length == 0 || offset + length > total_length) break;

        switch (type) {
        case 0x0B: // Interface Association Descriptor
            if (length >= 8) {
                Serial.printf("           \xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 IAD: interfaces %u..%u grouped as one function [%s]\n",
                              raw[offset + 2],
                              (unsigned)(raw[offset + 2] + raw[offset + 3] - 1),
                              class_name(raw[offset + 4], raw[offset + 5]));
            }
            break;

        case USB_B_DESCRIPTOR_TYPE_INTERFACE:
            if (length >= 9) {
                const uint8_t cls = raw[offset + 5];
                const uint8_t sub = raw[offset + 6];
                Serial.printf("           \xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 Interface %u alt %u \xE2\x80\x94 class 0x%02X/0x%02X/0x%02X  [%s], %u endpoint(s)\n",
                              raw[offset + 2], raw[offset + 3], cls, sub, raw[offset + 7],
                              class_name(cls, sub), raw[offset + 4]);
                tally.total++;
                if (cls == 0x01 && sub == 0x01) tally.audio_control++;
                else if (cls == 0x01 && sub == 0x02) tally.audio_streaming++;
                else if (cls == 0x01 && sub == 0x03) {
                    tally.midi_streaming++;
                    Serial.println("           \xE2\x94\x82    *** MIDI STREAMING interface \xE2\x80\x94 capturable MIDI source ***");
                }
                else if (cls == 0x03) tally.hid++;
                else if (cls == 0xFF) tally.vendor++;
            }
            break;

        case USB_B_DESCRIPTOR_TYPE_ENDPOINT:
            if (length >= 7) {
                static const char *transfer_names[] = {"Control", "Isochronous", "Bulk", "Interrupt"};
                const uint8_t ep_addr = raw[offset + 2];
                const uint16_t mps = (uint16_t)(raw[offset + 4] | (raw[offset + 5] << 8)) & 0x7FF;
                Serial.printf("           \xE2\x94\x82    \xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 EP 0x%02X %-3s %-11s  MPS %u, bInterval %u\n",
                              ep_addr, (ep_addr & 0x80) ? "IN" : "OUT",
                              transfer_names[raw[offset + 3] & 0x03], mps, raw[offset + 6]);
                tally.endpoints++;
            }
            break;

        case 0x24: // class-specific interface descriptor (audio/MIDI function internals)
        case 0x25: // class-specific endpoint descriptor
            Serial.printf("           \xE2\x94\x82    \xC2\xB7 class-specific %s descriptor, subtype 0x%02X (%u bytes)\n",
                          type == 0x24 ? "interface" : "endpoint", raw[offset + 2], length);
            break;

        default:
            Serial.printf("           \xE2\x94\x82    \xC2\xB7 descriptor type 0x%02X (%u bytes)\n", type, length);
            break;
        }
        offset = (uint16_t)(offset + length);
    }

    Serial.println("--------------------------- PROBE VERDICT ---------------------------");
    Serial.printf("Interface descriptors (incl. alternates): %u | endpoints: %u\n",
                  tally.total, tally.endpoints);
    Serial.printf("Audio Control: %u | Audio Streaming: %u | MIDI Streaming: %u | HID: %u | Vendor: %u\n",
                  tally.audio_control, tally.audio_streaming, tally.midi_streaming,
                  tally.hid, tally.vendor);
    Serial.println("RESULT: ENUMERATION COMPLETE \xE2\x80\x94 VBUS, D+/D-, PHY, and control transfers all working.");
    if (tally.midi_streaming > 0) {
        Serial.println("MIDI STREAMING FOUND \xE2\x80\x94 this device is capturable as a MIDI source.");
    } else {
        Serial.println("No MIDI Streaming interface on this device (expected for a non-MIDI proxy;");
        Serial.println("the Roland TD-02 must show class 0x01/0x03 here).");
    }
    Serial.println("======================================================================");
    Serial.println();
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg) {
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        log_line("[usb] EVENT: NEW_DEV \xE2\x80\x94 device attached at address %u",
                 event_msg->new_dev.address);
        s_open_address = event_msg->new_dev.address;
        s_open_pending = true;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        log_line("[usb] EVENT: DEV_GONE \xE2\x80\x94 device removed");
        if (event_msg->dev_gone.dev_hdl == s_device_handle) {
            s_close_pending = true;
        }
        break;
    default:
        break;
    }
}

// Open/close are deferred out of the callback and run in the client task
// right after usb_host_client_handle_events() returns. Close is processed
// first so a fast unplug/replug (DEV_GONE + NEW_DEV pending together) works.
static void process_pending_actions(void) {
    if (s_close_pending) {
        s_close_pending = false;
        if (s_device_handle != nullptr) {
            const esp_err_t err = usb_host_device_close(s_client_handle, s_device_handle);
            s_device_handle = nullptr;
            log_line("[usb] device closed (%s) \xE2\x80\x94 ready for replug", esp_err_to_name(err));
        }
    }

    if (s_open_pending) {
        s_open_pending = false;
        if (s_device_handle != nullptr) {
            log_line("[usb] NOTE: a device is already open; ignoring new device at address %u",
                     s_open_address);
            return;
        }
        const esp_err_t err = usb_host_device_open(s_client_handle, s_open_address, &s_device_handle);
        if (err != ESP_OK) {
            log_line("[usb] ERROR: open of address %u FAILED: %s",
                     s_open_address, esp_err_to_name(err));
            s_device_handle = nullptr;
            return;
        }
        log_line("[usb] device opened at address %u", s_open_address);
        print_device_report(s_device_handle);
    }
}

// Daemon task: usb_host_lib_handle_events() drives the hub/enumeration state
// machine. Without this loop the stack never enumerates and NEW_DEV never fires.
static void usb_lib_task(void *param) {
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    log_line("[usb] installing USB host stack...");
    const esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        log_line("[usb] FATAL: usb_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }
    log_line("[usb] host stack installed");
    s_host_installed = true;

    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            log_line("[usb] lib event: NO_CLIENTS \xE2\x80\x94 freeing all devices");
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            log_line("[usb] lib event: ALL_FREE \xE2\x80\x94 all devices released");
        }
    }
}

static void usb_client_task(void *param) {
    while (!s_host_installed) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = nullptr,
        },
    };

    const esp_err_t err = usb_host_client_register(&client_config, &s_client_handle);
    if (err != ESP_OK) {
        log_line("[usb] FATAL: usb_host_client_register failed: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }
    log_line("[usb] client registered \xE2\x80\x94 waiting for a device on the OTG port...");

    while (true) {
        usb_host_client_handle_events(s_client_handle, portMAX_DELAY);
        process_pending_actions();
    }
}

void setup() {
    Serial.begin(115200);
    delay(250);
    Serial.println();
    Serial.println("====================================================================");
    Serial.println(" e-drummer USB HOST ENUMERATION PROBE");
    Serial.printf(" Build: %s %s\n", __DATE__, __TIME__);
    Serial.println(" Experiment 2: VBUS + enumeration (td02-vbus-enumeration-experiment.md)");
    Serial.println(" Monitor on the UART/COM port; device on the native USB-OTG port.");
    Serial.println("====================================================================");

    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(usb_client_task, "usb_client", 6144, nullptr, 4, nullptr, 0);
}

void loop() {
    static uint32_t last_heartbeat = 0;
    if (millis() - last_heartbeat >= 10000) {
        last_heartbeat = millis();
        log_line("[hb] alive \xE2\x80\x94 %s",
                 s_device_handle != nullptr ? "device open" : "no device; waiting for attach");
    }
    delay(100);
}