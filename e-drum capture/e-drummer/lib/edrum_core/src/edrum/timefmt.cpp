#include "edrum/timefmt.h"

#include <stdio.h>
#include <string.h>

namespace edrum {

namespace {

bool valid_dt(const DateTime& dt) {
    if (dt.year < 1970 || dt.year > 9999) return false;
    if (dt.month < 1 || dt.month > 12) return false;
    if (dt.day < 1 || dt.day > 31) return false;
    if (dt.hour > 23 || dt.minute > 59 || dt.second > 59) return false;
    if (dt.tz_offset_min < -24 * 60 || dt.tz_offset_min > 24 * 60) return false;
    return true;
}

}  // namespace

size_t format_iso(const DateTime& dt, char* out, size_t cap) {
    if (cap < 26) return 0;
    const int off = dt.tz_offset_min;
    const char sign = off < 0 ? '-' : '+';
    const int aoff = off < 0 ? -off : off;
    const int n = snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
                           (int)dt.year, (int)dt.month, (int)dt.day, (int)dt.hour,
                           (int)dt.minute, (int)dt.second, sign, aoff / 60, aoff % 60);
    return (n == 25) ? (size_t)n : 0;
}

size_t session_filename(const DateTime& dt, const char* session_id, char* out, size_t cap) {
    char id8[9];
    size_t idlen = session_id ? strlen(session_id) : 0;
    if (idlen > 8) idlen = 8;
    memcpy(id8, session_id, idlen);
    id8[idlen] = '\0';
    const int n = snprintf(out, cap, "%04d%02d%02dT%02d%02d%02d_%s.jsonl",
                           (int)dt.year, (int)dt.month, (int)dt.day, (int)dt.hour,
                           (int)dt.minute, (int)dt.second, id8);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

namespace {

bool two_digits(const char* s, int* out) {
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return false;
    *out = (s[0] - '0') * 10 + (s[1] - '0');
    return true;
}

}  // namespace

bool parse_iso(const char* s, DateTime& out) {
    if (s == nullptr) return false;
    const size_t len = strlen(s);
    if (len != 19 && len != 20 && len != 25) return false;

    int y = 0;
    for (int i = 0; i < 4; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        y = y * 10 + (s[i] - '0');
    }
    int mo, d, h, mi, se;
    if (s[4] != '-' || !two_digits(s + 5, &mo)) return false;
    if (s[7] != '-' || !two_digits(s + 8, &d)) return false;
    if (s[10] != 'T' || !two_digits(s + 11, &h)) return false;
    if (s[13] != ':' || !two_digits(s + 14, &mi)) return false;
    if (s[16] != ':' || !two_digits(s + 17, &se)) return false;

    int off = 0;
    if (len == 20) {
        if (s[19] != 'Z') return false;
    } else if (len == 25) {
        int oh, om;
        if ((s[19] != '+' && s[19] != '-') || !two_digits(s + 20, &oh)) return false;
        if (s[22] != ':' || !two_digits(s + 23, &om)) return false;
        if (om > 59) return false;
        off = oh * 60 + om;
        if (s[19] == '-') off = -off;
    }

    DateTime dt;
    dt.year = (int16_t)y;
    dt.month = (uint8_t)mo;
    dt.day = (uint8_t)d;
    dt.hour = (uint8_t)h;
    dt.minute = (uint8_t)mi;
    dt.second = (uint8_t)se;
    dt.tz_offset_min = (int16_t)off;
    if (!valid_dt(dt)) return false;
    out = dt;
    return true;
}

// Howard Hinnant's civil-days algorithms (public domain derivation).
namespace {

int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                          // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;     // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;              // [0, 146096]
    return era * 146097 + (int64_t)doe - 719468;
}

void civil_from_days(int64_t z, int* y_out, unsigned* m_out, unsigned* d_out) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);                        // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int y = (int)(yoe) + (int)(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);             // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                                  // [0, 11]
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;                          // [1, 31]
    const unsigned m = mp + (mp < 10 ? 3 : (unsigned)-9);                     // [1, 12]
    *y_out = y + (m <= 2);
    *m_out = m;
    *d_out = d;
}

}  // namespace

int64_t datetime_to_epoch(const DateTime& dt) {
    const int64_t days = days_from_civil(dt.year, dt.month, dt.day);
    int64_t sec = days * 86400 + (int64_t)dt.hour * 3600 + (int64_t)dt.minute * 60 + dt.second;
    sec -= (int64_t)dt.tz_offset_min * 60;  // local -> UTC
    return sec;
}

void epoch_to_datetime(int64_t epoch_sec, int16_t tz_offset_min, DateTime& out) {
    const int64_t local = epoch_sec + (int64_t)tz_offset_min * 60;
    int64_t days = local / 86400;
    int64_t rem = local % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    int y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    out.year = (int16_t)y;
    out.month = (uint8_t)m;
    out.day = (uint8_t)d;
    out.hour = (uint8_t)(rem / 3600);
    out.minute = (uint8_t)((rem % 3600) / 60);
    out.second = (uint8_t)(rem % 60);
    out.tz_offset_min = tz_offset_min;
}

void uuid4_hex(const uint8_t bytes[16], char out[33]) {
    uint8_t b[16];
    memcpy(b, bytes, 16);
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);  // RFC 4122 variant
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        out[i * 2] = hex[b[i] >> 4];
        out[i * 2 + 1] = hex[b[i] & 0x0F];
    }
    out[32] = '\0';
}

}  // namespace edrum
