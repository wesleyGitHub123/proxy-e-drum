#include "edrum/config.h"

#include <stdlib.h>
#include <string.h>

namespace edrum {

namespace {

// Meta strings must survive JSON quoting untouched and stay shell-friendly:
// printable ASCII, no '"' or '\'. (The serializer could escape them, but a
// config value that needs escaping is a typo, not a feature.)
bool valid_name(const char* s, size_t len) {
    if (len == 0 || len > kNameMax) return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
    }
    return true;
}

bool parse_long(const char* s, long* out) {
    char* end = nullptr;
    const long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    *out = v;
    return true;
}

bool parse_bool(const char* s, bool* out) {
    if (strcmp(s, "1") == 0 || strcmp(s, "true") == 0 || strcmp(s, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, "0") == 0 || strcmp(s, "false") == 0 || strcmp(s, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

void trim(char** begin, char** end) {
    while (*begin < *end && (**begin == ' ' || **begin == '\t' || **begin == '\r')) ++*begin;
    while (*end > *begin && ((*end)[-1] == ' ' || (*end)[-1] == '\t' || (*end)[-1] == '\r')) --*end;
}

bool apply(Config& c, const char* key, const char* val) {
    const size_t vlen = strlen(val);
    long n = 0;

    if (strcmp(key, "user_id") == 0) {
        if (!valid_name(val, vlen)) return false;
        memcpy(c.user_id, val, vlen + 1);
        return true;
    }
    if (strcmp(key, "kit_profile_id") == 0) {
        if (strcmp(val, "null") == 0) {
            c.kit_profile_id[0] = '\0';
            return true;
        }
        if (!valid_name(val, vlen)) return false;
        memcpy(c.kit_profile_id, val, vlen + 1);
        return true;
    }
    if (strcmp(key, "calibration_offset_ms") == 0) {
        if (strcmp(val, "null") == 0) {
            c.has_calibration = false;
            return true;
        }
        if (!parse_long(val, &n) || n < -100000 || n > 100000) return false;
        c.has_calibration = true;
        c.calibration_offset_ms = (int32_t)n;
        return true;
    }
    if (strcmp(key, "tz_offset_min") == 0) {
        if (!parse_long(val, &n) || n < -24 * 60 || n > 24 * 60) return false;
        c.tz_offset_min = (int16_t)n;
        return true;
    }
    if (strcmp(key, "click_bpm") == 0) {
        if (!parse_long(val, &n) || n < 20 || n > 400) return false;
        c.click_bpm = (uint16_t)n;
        return true;
    }
    if (strcmp(key, "click_subdiv") == 0) {
        if (!parse_long(val, &n) || n < 1 || n > 16) return false;
        c.click_subdiv = (uint8_t)n;
        return true;
    }
    if (strcmp(key, "pause_after_ms") == 0) {
        if (!parse_long(val, &n) || n < 500 || n > 60000) return false;
        c.pause_after_ms = (uint32_t)n;
        return true;
    }
    if (strcmp(key, "idle_end_ms") == 0) {
        if (!parse_long(val, &n) || n < 5000 || n > 3600000) return false;
        c.idle_end_ms = (uint32_t)n;
        return true;
    }
    if (strcmp(key, "gesture_enable") == 0) {
        return parse_bool(val, &c.gesture_enable);
    }
    if (strcmp(key, "gesture_note_a") == 0) {
        if (!parse_long(val, &n) || n < 0 || n > 127) return false;
        c.gesture_note_a = (uint8_t)n;
        return true;
    }
    if (strcmp(key, "gesture_note_b") == 0) {
        if (!parse_long(val, &n) || n < 0 || n > 127) return false;
        c.gesture_note_b = (uint8_t)n;
        return true;
    }
    return false;  // unknown key — caller counts it separately
}

bool known_key(const char* key) {
    static const char* keys[] = {
        "user_id",        "kit_profile_id", "calibration_offset_ms", "tz_offset_min",
        "click_bpm",      "click_subdiv",   "pause_after_ms",        "idle_end_ms",
        "gesture_enable", "gesture_note_a", "gesture_note_b",
    };
    for (const char* k : keys) {
        if (strcmp(key, k) == 0) return true;
    }
    return false;
}

}  // namespace

ConfigParseStats parse_config(const char* text, size_t len, Config& io) {
    ConfigParseStats stats;
    size_t pos = 0;
    while (pos < len) {
        size_t eol = pos;
        while (eol < len && text[eol] != '\n') ++eol;

        char line[128];
        size_t llen = eol - pos;
        if (llen >= sizeof(line)) llen = sizeof(line) - 1;
        memcpy(line, text + pos, llen);
        line[llen] = '\0';
        pos = eol + 1;

        char* b = line;
        char* e = line + llen;
        trim(&b, &e);
        *e = '\0';
        if (b == e || *b == '#') continue;

        char* eq = strchr(b, '=');
        if (eq == nullptr) {
            ++stats.invalid;
            continue;
        }
        char* kb = b;
        char* ke = eq;
        char* vb = eq + 1;
        char* ve = e;
        trim(&kb, &ke);
        trim(&vb, &ve);
        *ke = '\0';
        *ve = '\0';
        if (kb == ke) {
            ++stats.invalid;
            continue;
        }

        if (!known_key(kb)) {
            ++stats.unknown;
        } else if (apply(io, kb, vb)) {
            ++stats.applied;
        } else {
            ++stats.invalid;
        }
    }
    return stats;
}

}  // namespace edrum
