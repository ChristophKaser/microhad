#include "syslog_log_reader.h"
#include "json_util.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>
#include <sys/types.h>
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <unistd.h>

namespace {
// Independently implemented from the IPC format documented in
// docs/syslog-ipc.md. No BusyBox implementation code is included here.
const long SyslogIpcKey = 0x414e4547L;
const unsigned long MaximumSegmentBytes = 16UL * 1024UL * 1024UL;
const unsigned long HeaderBytes = 8;

union SemaphoreArgument {
    int value;
    struct semid_ds* metadata;
    unsigned short* values;
};

bool changeReaderCount(int semaphore, short change) {
    struct sembuf operation;
    operation.sem_num = 0;
    operation.sem_op = change;
    operation.sem_flg = IPC_NOWAIT | SEM_UNDO;
    for (unsigned int attempt = 0; attempt < 20; ++attempt) {
        if (semop(semaphore, &operation, 1) == 0) return true;
        if (errno != EINTR) return false;
    }
    return false;
}

bool acquireSnapshot(int semaphore) {
    struct sembuf operations[2];
    operations[0].sem_num = 1;
    operations[0].sem_op = 0;
    operations[0].sem_flg = IPC_NOWAIT;
    operations[1].sem_num = 0;
    operations[1].sem_op = 1;
    operations[1].sem_flg = IPC_NOWAIT | SEM_UNDO;
    // Every syscall is nonblocking; retries allow at most 100 ms of sleeping.
    for (unsigned int attempt = 0; attempt < 20; ++attempt) {
        if (semop(semaphore, operations, 2) == 0) return true;
        if (errno != EAGAIN && errno != EINTR) return false;
        usleep(5000);
    }
    return false;
}

bool collectSyslogLines(long key, std::vector<std::string>* lines) {
    if (lines == 0) return false;
    lines->clear();
    int segment = shmget(static_cast<key_t>(key), 0, 0);
    int semaphore = semget(static_cast<key_t>(key), 2, 0);
    struct shmid_ds segmentInfo;
    struct semid_ds semaphoreInfo;
    SemaphoreArgument argument;
    argument.metadata = &semaphoreInfo;
    if (segment < 0 || semaphore < 0 || shmctl(segment, IPC_STAT, &segmentInfo) != 0 ||
        semctl(semaphore, 0, IPC_STAT, argument) != 0 || semaphoreInfo.sem_nsems != 2 ||
        segmentInfo.shm_segsz <= HeaderBytes + 1 || segmentInfo.shm_segsz > MaximumSegmentBytes) return false;
    // Allocate before locking so the writer is excluded only during the copy.
    std::vector<char> snapshot(segmentInfo.shm_segsz - HeaderBytes);
    void* mapping = shmat(segment, 0, SHM_RDONLY);
    if (mapping == reinterpret_cast<void*>(-1)) return false;
    if (!acquireSnapshot(semaphore)) { shmdt(mapping); return false; }
    int32_t capacity = 0, cursor = 0;
    const char* bytes = static_cast<const char*>(mapping);
    memcpy(&capacity, bytes, sizeof(capacity));
    memcpy(&cursor, bytes + sizeof(capacity), sizeof(cursor));
    bool valid = capacity > 0 && static_cast<unsigned long>(capacity) == snapshot.size() - 1 &&
        cursor >= 0 && cursor < capacity && bytes[HeaderBytes + capacity] == '\0' &&
        shmget(static_cast<key_t>(key), 0, 0) == segment &&
        semget(static_cast<key_t>(key), 2, 0) == semaphore;
    if (valid) memcpy(&snapshot[0], bytes + HeaderBytes, snapshot.size());
    bool released = changeReaderCount(semaphore, -1);
    shmdt(mapping);
    if (!valid || !released) return false;
    unsigned long newestEnd = cursor == 0 ? static_cast<unsigned long>(capacity - 1) :
        static_cast<unsigned long>(cursor - 1);
    if (snapshot[newestEnd] != '\0') return false;
    // Walk one revolution in chronological order. The cursor may cut through
    // the oldest overwritten record: discard that fragment through its NUL.
    bool boundary = false;
    std::string record;
    unsigned long position = static_cast<unsigned long>(cursor);
    for (unsigned long visited = 0; visited < static_cast<unsigned long>(capacity); ++visited) {
        char character = snapshot[position];
        if (character == '\0') {
            if (boundary && !record.empty()) {
                // syslog messages normally end with LF; retain the API's line
                // representation even when an IPC record contains several lines.
                std::string::size_type begin = 0;
                while (begin < record.size()) {
                    std::string::size_type end = record.find('\n', begin);
                    if (end == std::string::npos) end = record.size();
                    if (end > begin) lines->push_back(record.substr(begin, end - begin));
                    begin = end + 1;
                }
            }
            boundary = true;
            record.clear();
        } else if (boundary) record += character;
        if (++position == static_cast<unsigned long>(capacity)) position = 0;
    }
    if (!record.empty()) { lines->clear(); return false; }
    return true;
}

long timestampOf(const std::string& line) {
    char month[4] = {0, 0, 0, 0};
    int day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(line.c_str(), "%3s %d %d:%d:%d", month, &day, &hour, &minute, &second) != 5) return 0;
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int monthIndex = -1;
    for (int i = 0; i < 12; ++i) if (strcmp(month, months[i]) == 0) monthIndex = i;
    if (monthIndex < 0 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return 0;
    time_t now = time(0);
    struct tm current;
    if (localtime_r(&now, &current) == 0) return 0;
    struct tm parsed = current;
    parsed.tm_mon = monthIndex; parsed.tm_mday = day;
    parsed.tm_hour = hour; parsed.tm_min = minute; parsed.tm_sec = second; parsed.tm_isdst = -1;
    time_t result = mktime(&parsed);
    if (result > now + 86400) { parsed.tm_year -= 1; result = mktime(&parsed); }
    return static_cast<long>(result);
}

std::string apiLine(const std::string& line) {
    long timestamp = timestampOf(line);
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%ld ", timestamp);
    return std::string(prefix) + line;
}

std::string linesJson(const std::vector<std::string>& lines, unsigned long begin, unsigned long end) {
    std::ostringstream json;
    json << "{\"lines\":[";
    for (unsigned long i = begin; i < end; ++i) {
        if (i != begin) json << ',';
        json << '\"' << jsonEscape(apiLine(lines[i])) << '\"';
    }
    json << "]}";
    return json.str();
}
}

bool SyslogLogReader::readJson(std::string* json) {
    std::vector<std::string> lines;
    if (json == 0 || !collectSyslogLines(SyslogIpcKey, &lines)) return false;
    *json = linesJson(lines, 0, static_cast<unsigned long>(lines.size()));
    return true;
}

bool SyslogLogReader::readJson(unsigned long since, std::string* json) {
    std::vector<std::string> lines;
    if (json == 0 || !collectSyslogLines(SyslogIpcKey, &lines)) return false;
    std::ostringstream result;
    result << "{\"lines\":[";
    bool first = true;
    for (unsigned long i = 0; i < lines.size(); ++i) {
        if (static_cast<unsigned long>(timestampOf(lines[i])) < since) continue;
        if (!first) result << ',';
        first = false;
        result << '\"' << jsonEscape(apiLine(lines[i])) << '\"';
    }
    result << "]}";
    *json = result.str();
    return true;
}

bool SyslogLogReader::readPage(unsigned long limit, unsigned long before, std::string* json) {
    std::vector<std::string> lines;
    if (json == 0 || limit == 0 || limit > 500 || !collectSyslogLines(SyslogIpcKey, &lines)) return false;
    unsigned long count = static_cast<unsigned long>(lines.size());
    unsigned long end = before == 0 || before > count ? count : before;
    unsigned long begin = end > limit ? end - limit : 0;
    *json = linesJson(lines, begin, end);
    if (begin == 0) {
        json->insert(json->size() - 1, ",\"next_before\":0,\"has_more\":false");
    } else {
        std::ostringstream fields;
        fields << ",\"next_before\":" << begin << ",\"has_more\":true";
        json->insert(json->size() - 1, fields.str());
    }
    return true;
}

#ifdef MICROHAD_TESTING
bool SyslogLogReader::readLinesForTest(long key, std::vector<std::string>* lines) {
    return collectSyslogLines(key, lines);
}
#endif
