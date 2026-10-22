#ifndef MICROHAD_PROC_STATS_H
#define MICROHAD_PROC_STATS_H

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <stdint.h>

struct ProcessStat {
    uint64_t group, cpuTicks, startTicks, rssPages;
    std::string name;
    ProcessStat() : group(0), cpuTicks(0), startTicks(0), rssPages(0) {}
};

// comm may contain spaces and parentheses. All positional fields start after
// its final closing parenthesis; unrelated signed fields are only skipped.
inline bool parseProcessStat(const char* text, ProcessStat* result) {
    if (text == 0 || result == 0) return false;
    const char* open = strchr(text, '(');
    const char* close = strrchr(text, ')');
    if (open == 0 || close == 0 || open >= close || close[1] != ' ') return false;
    ProcessStat parsed;
    parsed.name.assign(open + 1, close - open - 1);
    const char* cursor = close + 2;
    for (int field = 3; field <= 24; ++field) {
        while (*cursor == ' ') ++cursor;
        const char* end = cursor;
        while (*end && *end != ' ' && *end != '\n') ++end;
        if (end == cursor) return false;
        if (field == 5 || field == 14 || field == 15 || field == 22 || field == 24) {
            if (*cursor < '0' || *cursor > '9') return false;
            errno = 0;
            char* numberEnd = 0;
            uint64_t value = strtoull(cursor, &numberEnd, 10);
            if (errno || numberEnd != end) return false;
            if (field == 5) parsed.group = value;
            else if (field == 14 || field == 15) parsed.cpuTicks += value;
            else if (field == 22) parsed.startTicks = value;
            else parsed.rssPages = value;
        }
        cursor = end;
    }
    *result = parsed;
    return true;
}

#endif
