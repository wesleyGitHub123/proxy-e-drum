#include "edrum/console_cmd.h"

#include <stdlib.h>
#include <string.h>

namespace edrum {

namespace {

// Tokenize in place (max 4 tokens). Returns token count.
int tokenize(char* s, char* tok[], int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
        if (!*s) break;
        tok[n++] = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') ++s;
        if (*s) *s++ = '\0';
    }
    return n;
}

bool to_u32(const char* s, uint32_t* out, uint32_t lo, uint32_t hi) {
    char* end = nullptr;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v < lo || v > hi) return false;
    *out = (uint32_t)v;
    return true;
}

bool valid_ref(const char* s) {
    const size_t len = strlen(s);
    if (len == 0 || len > kProfileRefMax) return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
    }
    return true;
}

}  // namespace

const char* console_help() {
    return
        "commands:\n"
        "  stats                    health/experiment counters\n"
        "  time                     show wall-clock time\n"
        "  settime <iso8601>        e.g. settime 2026-07-10T21:30:00+08:00\n"
        "  click <bpm> [subdiv]     start the click (device = tempo authority)\n"
        "  click off                stop the click\n"
        "  grid start | grid end    declare a graded span (needs running click)\n"
        "  bookmark                 drop a bookmark\n"
        "  enroll <name>            start groove enrollment span\n"
        "  enroll end               end enrollment span\n"
        "  end                      end the session now\n"
        "  burst <count> [hz]       synthetic event burst (Experiment 3)\n"
        "  help                     this text";
}

Command parse_command(const char* line) {
    Command cmd;
    char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* tok[4];
    const int n = tokenize(buf, tok, 4);
    if (n == 0) return cmd;  // Kind::None

    auto invalid = [&cmd](const char* why) -> Command {
        cmd.kind = Command::Kind::Invalid;
        cmd.error = why;
        return cmd;
    };

    if (strcmp(tok[0], "help") == 0) {
        cmd.kind = Command::Kind::Help;
    } else if (strcmp(tok[0], "stats") == 0) {
        cmd.kind = Command::Kind::Stats;
    } else if (strcmp(tok[0], "time") == 0) {
        cmd.kind = Command::Kind::Time;
    } else if (strcmp(tok[0], "settime") == 0) {
        if (n != 2 || !parse_iso(tok[1], cmd.dt)) {
            return invalid("usage: settime YYYY-MM-DDTHH:MM:SS[+HH:MM]");
        }
        cmd.kind = Command::Kind::SetTime;
    } else if (strcmp(tok[0], "click") == 0) {
        if (n >= 2 && strcmp(tok[1], "off") == 0) {
            cmd.kind = Command::Kind::ClickStop;
        } else {
            uint32_t bpm = 0, subdiv = 4;
            if (n < 2 || !to_u32(tok[1], &bpm, 20, 400)) {
                return invalid("usage: click <bpm 20-400> [subdiv 1-16] | click off");
            }
            if (n >= 3 && !to_u32(tok[2], &subdiv, 1, 16)) {
                return invalid("subdiv must be 1-16");
            }
            cmd.kind = Command::Kind::ClickStart;
            cmd.bpm = (uint16_t)bpm;
            cmd.subdiv = (uint8_t)subdiv;
        }
    } else if (strcmp(tok[0], "grid") == 0) {
        if (n == 2 && strcmp(tok[1], "start") == 0) {
            cmd.kind = Command::Kind::GridStart;
        } else if (n == 2 && strcmp(tok[1], "end") == 0) {
            cmd.kind = Command::Kind::GridEnd;
        } else {
            return invalid("usage: grid start | grid end");
        }
    } else if (strcmp(tok[0], "bookmark") == 0) {
        cmd.kind = Command::Kind::Bookmark;
    } else if (strcmp(tok[0], "enroll") == 0) {
        if (n == 2 && strcmp(tok[1], "end") == 0) {
            cmd.kind = Command::Kind::EnrollEnd;
        } else if (n == 2 && valid_ref(tok[1])) {
            cmd.kind = Command::Kind::EnrollStart;
            strncpy(cmd.ref, tok[1], kProfileRefMax);
        } else {
            return invalid("usage: enroll <name> | enroll end");
        }
    } else if (strcmp(tok[0], "end") == 0) {
        cmd.kind = Command::Kind::EndSession;
    } else if (strcmp(tok[0], "burst") == 0) {
        uint32_t count = 0, hz = 500;
        if (n < 2 || !to_u32(tok[1], &count, 1, 1000000)) {
            return invalid("usage: burst <count> [hz]");
        }
        if (n >= 3 && !to_u32(tok[2], &hz, 1, 5000)) {
            return invalid("hz must be 1-5000");
        }
        cmd.kind = Command::Kind::Burst;
        cmd.burst_count = count;
        cmd.burst_hz = (uint16_t)hz;
    } else {
        return invalid("unknown command (try: help)");
    }
    return cmd;
}

}  // namespace edrum
