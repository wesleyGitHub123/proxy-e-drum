#include "edrum/serialize.h"

#include <stdio.h>
#include <string.h>

namespace edrum {

namespace {

// Bounds-checked line builder. Any overflow poisons the writer; the caller
// sees length 0 and treats it as a serialization failure (counted upstream).
struct Out {
    char* buf;
    size_t cap;
    size_t len = 0;
    bool ok = true;

    void ch(char c) {
        if (!ok || len + 1 > cap) {
            ok = false;
            return;
        }
        buf[len++] = c;
    }

    void raw(const char* s) {
        const size_t n = strlen(s);
        if (!ok || len + n > cap) {
            ok = false;
            return;
        }
        memcpy(buf + len, s, n);
        len += n;
    }

  void integer(int64_t v) {
        char tmp[24];
        int idx = 24;
        const bool neg = v < 0;
        uint64_t uv = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
        do {
            tmp[--idx] = char('0' + (uv % 10));
            uv /= 10;
        } while (uv > 0);
        if (neg) tmp[--idx] = '-';
        const size_t n = (size_t)(24 - idx);
        if (!ok || len + n > cap) {
            ok = false;
            return;
        }
        memcpy(buf + len, tmp + idx, n);
        len += n;
    }

    void quoted(const char* s) {
        ch('"');
        for (const char* p = s; ok && *p; ++p) {
            const unsigned char c = (unsigned char)*p;
            switch (c) {
                case '"': raw("\\\""); break;
                case '\\': raw("\\\\"); break;
                case '\b': raw("\\b"); break;
                case '\t': raw("\\t"); break;
                case '\n': raw("\\n"); break;
                case '\f': raw("\\f"); break;
                case '\r': raw("\\r"); break;
                default:
                    if (c < 0x20) {
                        char tmp[8];
                        snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)c);
                        raw(tmp);
                    } else {
                        ch((char)c);  // UTF-8 bytes pass through (ensure_ascii=False)
                    }
            }
        }
        ch('"');
    }

    // `,"key":` — every key after the first; first key uses key0.
    void key0(const char* k) {
        quoted(k);
        ch(':');
    }
    void key(const char* k) {
        ch(',');
        quoted(k);
        ch(':');
    }

    size_t finish() {
        ch('\n');
        return ok ? len : 0;
    }
};

void open_typed(Out& o, const char* type) {
    o.ch('{');
    o.key0("type");
    o.quoted(type);
}

void t_field(Out& o, uint32_t t) {
    o.key("t");
    o.integer((int64_t)t);
}

// ctrl msg payload for a raw (non-sysex) MIDI message, mido-style names,
// key order: "type" first, remaining keys sorted alphabetically
// (phase0-plan micro-decision 1).
bool msg_object(Out& o, const MidiMsg& m) {
    o.ch('{');
    const uint8_t hi = m.type_nibble();
    if (m.is_channel_message()) {
        const int ch = m.channel();
        switch (hi) {
            case 0x80:
                o.key0("type"); o.quoted("note_off");
                o.key("channel"); o.integer(ch);
                o.key("note"); o.integer(m.data1);
                o.key("velocity"); o.integer(m.data2);
                break;
            case 0x90:  // reaches ctrl only as velocity-0 (micro-decision 3)
                o.key0("type"); o.quoted("note_on");
                o.key("channel"); o.integer(ch);
                o.key("note"); o.integer(m.data1);
                o.key("velocity"); o.integer(m.data2);
                break;
            case 0xA0:
                o.key0("type"); o.quoted("polytouch");
                o.key("channel"); o.integer(ch);
                o.key("note"); o.integer(m.data1);
                o.key("value"); o.integer(m.data2);
                break;
            case 0xB0:
                o.key0("type"); o.quoted("control_change");
                o.key("channel"); o.integer(ch);
                o.key("control"); o.integer(m.data1);
                o.key("value"); o.integer(m.data2);
                break;
            case 0xC0:
                o.key0("type"); o.quoted("program_change");
                o.key("channel"); o.integer(ch);
                o.key("program"); o.integer(m.data1);
                break;
            case 0xD0:
                o.key0("type"); o.quoted("aftertouch");
                o.key("channel"); o.integer(ch);
                o.key("value"); o.integer(m.data1);
                break;
            case 0xE0:
                o.key0("type"); o.quoted("pitchwheel");
                o.key("channel"); o.integer(ch);
                // mido pitch = 14-bit value - 8192, range [-8192, 8191]
                o.key("pitch");
                o.integer((int64_t)(((int)m.data2 << 7) | m.data1) - 8192);
                break;
            default:
                return false;
        }
    } else {
        switch (m.status) {
            case 0xF1:
                o.key0("type"); o.quoted("quarter_frame");
                o.key("frame_type"); o.integer(m.data1 >> 4);
                o.key("frame_value"); o.integer(m.data1 & 0x0F);
                break;
            case 0xF2:
                o.key0("type"); o.quoted("songpos");
                o.key("pos"); o.integer(((int)m.data2 << 7) | m.data1);
                break;
            case 0xF3:
                o.key0("type"); o.quoted("song_select");
                o.key("song"); o.integer(m.data1);
                break;
            case 0xF6:
                o.key0("type"); o.quoted("tune_request");
                break;
            default:
                return false;  // realtime never reaches here (classifier drops it)
        }
    }
    o.ch('}');
    return true;
}

}  // namespace

size_t json_escape(const char* in, char* out, size_t cap) {
    Out o{out, cap};
    o.quoted(in);
    return o.ok ? o.len : 0;
}

size_t meta_line(const SessionStartP& s, const MetaStatic& ms, char* out, size_t cap) {
    Out o{out, cap};
    open_typed(o, "meta");
    o.key("schema_version");
    o.integer(kSchemaVersion);
    o.key("session_id");
    o.quoted(s.session_id);
    o.key("start_iso");
    o.quoted(s.start_iso);
    o.key("kit_profile_id");
    if (ms.kit_profile_id[0] == '\0') {
        o.raw("null");
    } else {
        o.quoted(ms.kit_profile_id);
    }
    o.key("user_id");
    o.quoted(ms.user_id);
    o.key("calibration_offset_ms");
    if (ms.has_calibration) {
        o.integer(ms.calibration_offset_ms);
    } else {
        o.raw("null");
    }
    o.ch('}');
    return o.finish();
}

size_t record_line(const CaptureRecord& rec, char* out, size_t cap) {
    Out o{out, cap};
    switch (rec.type) {
        case RecType::SessionStart:
            return 0;  // marker, not a line

        case RecType::Event:
            open_typed(o, "event");
            t_field(o, rec.t);
            o.key("note");
            o.integer(rec.u.event.note);
            o.key("velocity");
            o.integer(rec.u.event.velocity);
            o.key("channel");
            o.integer(rec.u.event.channel);
            o.ch('}');
            break;

        case RecType::CtrlMidi:
            open_typed(o, "ctrl");
            t_field(o, rec.t);
            o.key("msg");
            if (!msg_object(o, rec.u.ctrl.msg)) return 0;
            o.ch('}');
            break;

        case RecType::CtrlSysex: {
            open_typed(o, "ctrl");
            t_field(o, rec.t);
            o.key("msg");
            o.ch('{');
            o.key0("type");
            o.quoted("sysex");
            o.key("data");
            o.ch('[');
            for (uint8_t i = 0; i < rec.u.sysex.len; ++i) {
                if (i) o.ch(',');
                o.integer(rec.u.sysex.data[i]);
            }
            o.ch(']');
            o.ch('}');
            o.ch('}');
            break;
        }

        case RecType::GridStart:
            open_typed(o, "grid_start");
            t_field(o, rec.t);
            o.key("bpm");
            o.integer(rec.u.grid.bpm);
            o.key("subdiv");
            o.integer(rec.u.grid.subdiv);
            o.key("downbeat_t");
            o.integer(rec.u.grid.downbeat_t);
            o.ch('}');
            break;

        case RecType::GridEnd:
            open_typed(o, "grid_end");
            t_field(o, rec.t);
            o.ch('}');
            break;

        case RecType::Bookmark:
            open_typed(o, "bookmark");
            t_field(o, rec.t);
            o.ch('}');
            break;

        case RecType::EnrollStart:
            open_typed(o, "enroll_start");
            t_field(o, rec.t);
            o.key("profile_ref");
            o.quoted(rec.u.enroll.profile_ref);
            o.key("bpm");
            o.integer(rec.u.enroll.bpm);
            o.key("subdiv");
            o.integer(rec.u.enroll.subdiv);
            o.key("downbeat_t");
            o.integer(rec.u.enroll.downbeat_t);
            o.ch('}');
            break;

        case RecType::EnrollEnd:
            open_typed(o, "enroll_end");
            t_field(o, rec.t);
            o.ch('}');
            break;

        case RecType::SessionEnd:
            open_typed(o, "session_end");
            t_field(o, rec.t);
            o.ch('}');
            break;

        default:
            return 0;
    }
    return o.finish();
}

}  // namespace edrum
