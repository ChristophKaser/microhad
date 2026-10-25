#include "syslog_log_reader.h"
#include "json_util.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>
#include <pthread.h>
#include <tr1/memory>
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

struct LineRange {
    unsigned long offset, length;
    LineRange(unsigned long start, unsigned long bytes) : offset(start), length(bytes) {}
};

#ifdef MICROHAD_TESTING
typedef SyslogReadStats ReadStats;
#else
struct ReadStats {};
#endif

bool readSnapshot(long key, std::vector<char>* snapshot) {
    if (snapshot == 0) return false;
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
    snapshot->resize(segmentInfo.shm_segsz - HeaderBytes);
    void* mapping = shmat(segment, 0, SHM_RDONLY);
    if (mapping == reinterpret_cast<void*>(-1)) return false;
    if (!acquireSnapshot(semaphore)) { shmdt(mapping); return false; }
    int32_t capacity = 0, cursor = 0;
    const char* bytes = static_cast<const char*>(mapping);
    memcpy(&capacity, bytes, sizeof(capacity));
    memcpy(&cursor, bytes + sizeof(capacity), sizeof(cursor));
    bool valid = capacity > 0 && static_cast<unsigned long>(capacity) == snapshot->size() - 1 &&
        cursor >= 0 && cursor < capacity && bytes[HeaderBytes + capacity] == '\0' &&
        shmget(static_cast<key_t>(key), 0, 0) == segment &&
        semget(static_cast<key_t>(key), 2, 0) == semaphore;
    if (valid) {
        // Copy in chronological order, starting at the write cursor. These two
        // copies also make a physically wrapped record contiguous for parsing.
        unsigned long first = static_cast<unsigned long>(capacity - cursor);
        memcpy(&(*snapshot)[0], bytes + HeaderBytes + cursor, first);
        if (cursor != 0) memcpy(&(*snapshot)[first], bytes + HeaderBytes, cursor);
        (*snapshot)[capacity] = '\0';
    }
    bool released = changeReaderCount(semaphore, -1);
    shmdt(mapping);
    if (!valid || !released) return false;
    if ((*snapshot)[capacity - 1] != '\0') return false;
    return true;
}

bool indexLines(const std::vector<char>& snapshot, std::vector<LineRange>* lines, ReadStats* stats) {
#ifndef MICROHAD_TESTING
    (void)stats;
#endif
    lines->clear();
    unsigned long capacity = static_cast<unsigned long>(snapshot.size() - 1);
    const char* bytes = &snapshot[0];
    // Discard the possibly overwritten oldest fragment, through its first NUL.
    const char* boundary = static_cast<const char*>(memchr(bytes, '\0', capacity));
    if (boundary == 0) return false;
    unsigned long position = static_cast<unsigned long>(boundary - bytes) + 1;
    static const char zeros[64] = {0};
    while (position < capacity) {
        // A partially filled ring can contain hundreds of KiB of zero padding.
        // Skip that padding in blocks instead of creating empty record objects.
        while (capacity - position >= sizeof(zeros) &&
               memcmp(bytes + position, zeros, sizeof(zeros)) == 0) {
            position += sizeof(zeros);
#ifdef MICROHAD_TESTING
            if (stats != 0) ++stats->zeroBlocksSkipped;
#endif
        }
        while (position < capacity && bytes[position] == '\0') ++position;
        if (position == capacity) break;
        const char* terminator = static_cast<const char*>(memchr(bytes + position, '\0', capacity - position));
        if (terminator == 0) return false;
        unsigned long end = static_cast<unsigned long>(terminator - bytes);
        while (position < end) {
            const char* newline = static_cast<const char*>(memchr(bytes + position, '\n', end - position));
            unsigned long lineEnd = newline == 0 ? end : static_cast<unsigned long>(newline - bytes);
            if (lineEnd > position) {
                lines->push_back(LineRange(position, lineEnd - position));
#ifdef MICROHAD_TESTING
                if (stats != 0) ++stats->indexedLines;
#endif
            }
            position = lineEnd + 1;
        }
        position = end + 1;
    }
    return true;
}

bool leapYear(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

// A named timezone may contain historical offset changes even without DST.
// Only explicit fixed POSIX zones (or the system's UTC default) qualify.
bool fixedTimezone() {
    if (daylight != 0) return false;
    const char* zone = getenv("TZ");
    if (zone == 0) return timezone == 0 && tzname[0] != 0 &&
        (strcmp(tzname[0], "UTC") == 0 || strcmp(tzname[0], "GMT") == 0);
    if (*zone == '\0') return true;
    unsigned long letters = 0;
    while ((*zone >= 'A' && *zone <= 'Z') || (*zone >= 'a' && *zone <= 'z')) { ++zone; ++letters; }
    if (letters < 3) return false;
    if (*zone == '\0') return strcmp(tzname[0], "UTC") == 0 || strcmp(tzname[0], "GMT") == 0;
    if (*zone == '+' || *zone == '-') ++zone;
    unsigned int fields = 0;
    while (true) {
        if (*zone < '0' || *zone > '9') return false;
        while (*zone >= '0' && *zone <= '9') ++zone;
        if (*zone == '\0') return true;
        if (*zone++ != ':' || ++fields > 2) return false;
    }
}

struct TimestampContext {
    time_t now;
    struct tm current;
    bool valid, fixedOffset;
    int64_t yearStart;
    explicit TimestampContext(time_t observedAt) : now(observedAt),
        valid(localtime_r(&now, &current) != 0), fixedOffset(false), yearStart(0) {
        fixedOffset = valid && current.tm_isdst == 0 && fixedTimezone();
        if (fixedOffset) yearStart = static_cast<int64_t>(now) -
            static_cast<int64_t>(current.tm_yday) * 86400 - current.tm_hour * 3600 -
            current.tm_min * 60 - current.tm_sec;
    }
};

bool digit(char c) { return c >= '0' && c <= '9'; }

bool standardTimestamp(const char* bytes, unsigned long length, struct tm* parsed) {
    if (length < 15 || bytes[3] != ' ' || bytes[6] != ' ' || bytes[9] != ':' || bytes[12] != ':' ||
        (length > 15 && digit(bytes[15])) || (bytes[4] != ' ' && !digit(bytes[4]))) return false;
    const unsigned int digits[] = {5, 7, 8, 10, 11, 13, 14};
    for (unsigned int i = 0; i < sizeof(digits) / sizeof(digits[0]); ++i)
        if (!digit(bytes[digits[i]])) return false;
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int month = 0;
    while (month < 12 && memcmp(bytes, months + month * 3, 3) != 0) ++month;
    if (month == 12) return false;
    parsed->tm_mon = month;
    parsed->tm_mday = (bytes[4] == ' ' ? 0 : bytes[4] - '0') * 10 + bytes[5] - '0';
    parsed->tm_hour = (bytes[7] - '0') * 10 + bytes[8] - '0';
    parsed->tm_min = (bytes[10] - '0') * 10 + bytes[11] - '0';
    parsed->tm_sec = (bytes[13] - '0') * 10 + bytes[14] - '0';
    return parsed->tm_mday >= 1 && parsed->tm_mday <= 31 && parsed->tm_hour <= 23 &&
        parsed->tm_min <= 59 && parsed->tm_sec <= 60;
}

bool fixedTimestamp(const struct tm& parsed, const TimestampContext& context, long* timestamp) {
    if (!context.fixedOffset || parsed.tm_sec == 60) return false;
    static const int monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int year = context.current.tm_year + 1900;
    int64_t yearStart = context.yearStart;
    for (unsigned int attempt = 0; attempt < 2; ++attempt) {
        bool leap = leapYear(year);
        if (parsed.tm_mday > monthDays[parsed.tm_mon] + (parsed.tm_mon == 1 && leap ? 1 : 0)) return false;
        int days = parsed.tm_mday - 1;
        for (int month = 0; month < parsed.tm_mon; ++month) days += monthDays[month];
        if (leap && parsed.tm_mon > 1) ++days;
        int64_t result = yearStart + static_cast<int64_t>(days) * 86400 +
            parsed.tm_hour * 3600 + parsed.tm_min * 60 + parsed.tm_sec;
        if (attempt == 0 && result > static_cast<int64_t>(context.now) + 86400) {
            --year;
            yearStart -= static_cast<int64_t>(leapYear(year) ? 366 : 365) * 86400;
            continue;
        }
        if (result < LONG_MIN || result > LONG_MAX) return false;
        *timestamp = static_cast<long>(result);
        return true;
    }
    return false;
}


long rangeTimestamp(const std::vector<char>& snapshot, const LineRange& range,
                    const TimestampContext& context, ReadStats* stats) {
#ifndef MICROHAD_TESTING
    (void)stats;
#else
    if (stats != 0) ++stats->timestampConversions;
#endif
    if (!context.valid) return 0;
    struct tm parsed = context.current;
    if (standardTimestamp(&snapshot[range.offset], range.length, &parsed)) {
#ifdef MICROHAD_TESTING
        if (stats != 0) ++stats->fastTimestampParses;
#endif
        long timestamp;
        if (fixedTimestamp(parsed, context, &timestamp)) return timestamp;
    } else {
        // Normal syslog headers fit on the stack. Preserve the old sscanf behavior
        // for unusual long whitespace/header fields with a bounded fallback copy.
        char prefix[64];
        unsigned long length = range.length < sizeof(prefix) - 1 ? range.length : sizeof(prefix) - 1;
        memcpy(prefix, &snapshot[range.offset], length);
        prefix[length] = '\0';
        char month[4] = {0, 0, 0, 0};
        int day = 0, hour = 0, minute = 0, second = 0, consumed = 0;
        int matched = sscanf(prefix, "%3s %d %d:%d:%d%n", month, &day, &hour, &minute, &second, &consumed);
        if (range.length > length && (matched != 5 || consumed >= static_cast<int>(length))) {
            std::string header(&snapshot[range.offset], range.length);
            matched = sscanf(header.c_str(), "%3s %d %d:%d:%d", month, &day, &hour, &minute, &second);
        }
        if (matched != 5) return 0;
        static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        int monthIndex = -1;
        for (int i = 0; i < 12; ++i) if (strcmp(month, months[i]) == 0) monthIndex = i;
        if (monthIndex < 0 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return 0;
        parsed.tm_mon = monthIndex; parsed.tm_mday = day;
        parsed.tm_hour = hour; parsed.tm_min = minute; parsed.tm_sec = second;
    }
    parsed.tm_isdst = -1;
#ifdef MICROHAD_TESTING
    if (stats != 0) ++stats->calendarConversions;
#endif
    time_t result = mktime(&parsed);
    if (static_cast<int64_t>(result) > static_cast<int64_t>(context.now) + 86400) {
        parsed.tm_year -= 1;
#ifdef MICROHAD_TESTING
        if (stats != 0) ++stats->calendarConversions;
#endif
        result = mktime(&parsed);
    }
    return static_cast<long>(result);
}

void appendLine(std::ostringstream* json, const std::vector<char>& snapshot, const LineRange& range,
                long timestamp, bool* first, ReadStats* stats) {
#ifndef MICROHAD_TESTING
    (void)stats;
#else
    if (stats != 0) ++stats->materializedLines;
#endif
    if (!*first) *json << ',';
    *first = false;
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%ld ", timestamp);
    std::string line(prefix);
    line.append(&snapshot[range.offset], range.length);
    *json << '\"' << jsonEscape(line) << '\"';
}

// Retain one bounded immutable index. Compare the entire snapshot, including
// zero padding, so a wrapped cursor or a restarted syslog daemon cannot yield
// a stale hit. Readers keep their own shared lease while formatting output.
struct IndexedSnapshot { std::vector<char> bytes;std::vector<LineRange> lines; };
pthread_mutex_t indexCacheMutex=PTHREAD_MUTEX_INITIALIZER;
std::tr1::shared_ptr<const IndexedSnapshot> indexCache;
bool readIndexedSnapshot(long key,std::tr1::shared_ptr<const IndexedSnapshot>* result,ReadStats* stats) {
    std::tr1::shared_ptr<IndexedSnapshot> next(new IndexedSnapshot);
    if(!readSnapshot(key,&next->bytes))return false;
    const bool cacheable=next->bytes.size()<=512UL*1024UL+1;
    if(cacheable){
        pthread_mutex_lock(&indexCacheMutex);
        if(indexCache&&indexCache->bytes==next->bytes){
            *result=indexCache;
#ifdef MICROHAD_TESTING
            if(stats){stats->indexedLines=indexCache->lines.size();++stats->indexCacheHits;}
#endif
            pthread_mutex_unlock(&indexCacheMutex);return true;
        }
        pthread_mutex_unlock(&indexCacheMutex);
    }
    if(!indexLines(next->bytes,&next->lines,stats))return false;
    *result=next;
    if(cacheable&&next->lines.size()<=16384){pthread_mutex_lock(&indexCacheMutex);indexCache=next;pthread_mutex_unlock(&indexCacheMutex);}
    return true;
}

bool readRequest(long key, bool hasSince, unsigned long since, unsigned long limit, unsigned long before,
                 time_t now, std::string* json, ReadStats* stats) {
    if (json == 0 || limit > 500) return false;
#ifdef MICROHAD_TESTING
    if (stats != 0) *stats = SyslogReadStats();
#endif
    std::tr1::shared_ptr<const IndexedSnapshot> indexed;
    if(!readIndexedSnapshot(key,&indexed,stats))return false;
    const std::vector<char>& snapshot=indexed->bytes;
    const std::vector<LineRange>& lines=indexed->lines;
    unsigned long count = static_cast<unsigned long>(lines.size());
    unsigned long end = limit == 0 || before == 0 || before > count ? count : before;
    unsigned long begin = limit == 0 || end <= limit ? 0 : end - limit;
    TimestampContext context(now);
    std::ostringstream result;
    result << "{\"lines\":[";
    bool first = true;
    for (unsigned long i = begin; i < end; ++i) {
        long timestamp = rangeTimestamp(snapshot, lines[i], context, stats);
        if (limit == 0 && hasSince && static_cast<unsigned long>(timestamp) < since) continue;
        appendLine(&result, snapshot, lines[i], timestamp, &first, stats);
    }
    result << ']';
    if (limit != 0) result << ",\"next_before\":" << begin << ",\"has_more\":" << (begin == 0 ? "false" : "true");
    result << '}';
    *json = result.str();
    return true;
}
}

bool SyslogLogReader::readJson(std::string* json) {
    return readRequest(SyslogIpcKey, false, 0, 0, 0, time(0), json, 0);
}

bool SyslogLogReader::readJson(unsigned long since, std::string* json) {
    return readRequest(SyslogIpcKey, true, since, 0, 0, time(0), json, 0);
}

bool SyslogLogReader::readPage(unsigned long limit, unsigned long before, std::string* json) {
    return limit != 0 && readRequest(SyslogIpcKey, false, 0, limit, before, time(0), json, 0);
}

#ifdef MICROHAD_TESTING
bool SyslogLogReader::readLinesForTest(long key, std::vector<std::string>* lines) {
    if (lines == 0) return false;
    lines->clear();
    std::vector<char> snapshot;
    std::vector<LineRange> ranges;
    if (!readSnapshot(key, &snapshot) || !indexLines(snapshot, &ranges, 0)) return false;
    for (unsigned long i = 0; i < ranges.size(); ++i)
        lines->push_back(std::string(&snapshot[ranges[i].offset], ranges[i].length));
    return true;
}

bool SyslogLogReader::readRequestForTest(long key, bool hasSince, unsigned long since,
                                       unsigned long limit, unsigned long before, long now,
                                       std::string* json, SyslogReadStats* stats) {
    return readRequest(key, hasSince, since, limit, before, static_cast<time_t>(now), json, stats);
}
#endif
