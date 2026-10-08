#include "syslog_log_reader.h"
#include "json_util.h"

#include <cassert>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdint.h>
#include <string>
#include <vector>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <unistd.h>

namespace {
union SemValue { int value; struct semid_ds* metadata; unsigned short* values; };

class IpcFixture {
public:
    long key;
    int segment, semaphore;
    char* bytes;
    unsigned long capacity;
    explicit IpcFixture(unsigned long ringCapacity = 64) : key(0), segment(-1), semaphore(-1), bytes(0), capacity(ringCapacity) {
        for (unsigned int i = 0; i < 100; ++i) {
            key = 0x20000000L + static_cast<long>(getpid()) * 100 + i;
            segment = shmget(static_cast<key_t>(key), capacity + 9, IPC_CREAT | IPC_EXCL | 0600);
            if (segment >= 0) break;
        }
        assert(segment >= 0);
        semaphore = semget(static_cast<key_t>(key), 2, IPC_CREAT | IPC_EXCL | 0600);
        assert(semaphore >= 0);
        bytes = static_cast<char*>(shmat(segment, 0, 0));
        assert(bytes != reinterpret_cast<char*>(-1));
        reset();
    }
    ~IpcFixture() {
        shmdt(bytes); shmctl(segment, IPC_RMID, 0); semctl(semaphore, 0, IPC_RMID);
    }
    void reset() {
        memset(bytes, 0, capacity + 9);
        header(static_cast<int32_t>(capacity), 0);
        unsigned short values[2] = {0, 0};
        SemValue argument; argument.values = values;
        assert(semctl(semaphore, 0, SETALL, argument) == 0);
    }
    void header(int32_t capacity, int32_t cursor) {
        memcpy(bytes, &capacity, 4); memcpy(bytes + 4, &cursor, 4);
    }
    void ring(const std::string& chronological, unsigned int cursor) {
        assert(chronological.size() == capacity);
        for (unsigned int i = 0; i < capacity; ++i) bytes[8 + (cursor + i) % capacity] = chronological[i];
        header(static_cast<int32_t>(capacity), cursor);
    }
};

std::vector<std::string> referenceLines(const std::string& chronological) {
    std::vector<std::string> lines;
    bool boundary = false;
    std::string record;
    for (unsigned long i = 0; i < chronological.size(); ++i) {
        char c = chronological[i];
        if (c == '\0') {
            if (boundary && !record.empty()) {
                std::string::size_type begin = 0;
                while (begin < record.size()) {
                    std::string::size_type end = record.find('\n', begin);
                    if (end == std::string::npos) end = record.size();
                    if (end > begin) lines.push_back(record.substr(begin, end - begin));
                    begin = end + 1;
                }
            }
            boundary = true; record.clear();
        } else if (boundary) record += c;
    }
    return lines;
}

long referenceTimestamp(const std::string& line, time_t now) {
    char month[4] = {0}; int day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(line.c_str(), "%3s %d %d:%d:%d", month, &day, &hour, &minute, &second) != 5) return 0;
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int index = -1;
    for (int i = 0; i < 12; ++i) if (strcmp(month, months[i]) == 0) index = i;
    if (index < 0 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return 0;
    struct tm parsed; if (localtime_r(&now, &parsed) == 0) return 0;
    parsed.tm_mon = index; parsed.tm_mday = day;
    parsed.tm_hour = hour; parsed.tm_min = minute; parsed.tm_sec = second; parsed.tm_isdst = -1;
    time_t result = mktime(&parsed);
    if (result > now + 86400) { parsed.tm_year -= 1; result = mktime(&parsed); }
    return static_cast<long>(result);
}

std::string referenceJson(const std::vector<std::string>& lines, bool hasSince, unsigned long since,
                          unsigned long limit, unsigned long before, time_t now) {
    unsigned long count = static_cast<unsigned long>(lines.size());
    unsigned long end = limit == 0 || before == 0 || before > count ? count : before;
    unsigned long begin = limit == 0 || end <= limit ? 0 : end - limit;
    std::ostringstream output; output << "{\"lines\":["; bool first = true;
    for (unsigned long i = begin; i < end; ++i) {
        long timestamp = referenceTimestamp(lines[i], now);
        if (limit == 0 && hasSince && static_cast<unsigned long>(timestamp) < since) continue;
        if (!first) output << ',';
        first = false;
        char prefix[32]; snprintf(prefix, sizeof(prefix), "%ld ", timestamp);
        output << '\"' << jsonEscape(std::string(prefix) + lines[i]) << '\"';
    }
    output << ']';
    if (limit != 0) output << ",\"next_before\":" << begin << ",\"has_more\":" << (begin == 0 ? "false" : "true");
    output << '}'; return output.str();
}

void checkQueries(IpcFixture* ipc, const std::string& chronological, unsigned long cursor, time_t now) {
    ipc->ring(chronological, static_cast<unsigned int>(cursor));
    std::vector<std::string> expected = referenceLines(chronological), actual;
    assert(SyslogLogReader::readLinesForTest(ipc->key, &actual) && actual == expected);
    const unsigned long limits[] = {0, 1, 2, 10, 500};
    const unsigned long before[] = {0, 1, 2, static_cast<unsigned long>(expected.size()), 10000};
    for (unsigned long i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        for (unsigned long j = 0; j < sizeof(before) / sizeof(before[0]); ++j) {
            SyslogReadStats stats; std::string json;
            assert(SyslogLogReader::readRequestForTest(ipc->key, false, 0, limits[i], before[j], now, &json, &stats));
            assert(json == referenceJson(expected, false, 0, limits[i], before[j], now));
            assert(stats.indexedLines == expected.size());
            assert(stats.timestampConversions == stats.materializedLines);
            if (limits[i] != 0) assert(stats.materializedLines <= limits[i]);
        }
    }
    const unsigned long since[] = {0, static_cast<unsigned long>(now - 2), static_cast<unsigned long>(now),
                                   static_cast<unsigned long>(now + 86400)};
    for (unsigned long i = 0; i < sizeof(since) / sizeof(since[0]); ++i) {
        SyslogReadStats stats; std::string json;
        assert(SyslogLogReader::readRequestForTest(ipc->key, true, since[i], 0, 0, now, &json, &stats));
        assert(json == referenceJson(expected, true, since[i], 0, 0, now));
        assert(stats.timestampConversions == expected.size());
        assert(stats.materializedLines <= stats.timestampConversions);
    }
}

void queryTests() {
    // Fix TZ only for deterministic date/year tests; restore the caller's TZ.
    const char* original = getenv("TZ"); bool hadZone = original != 0;
    std::string savedZone = hadZone ? original : "";
    setenv("TZ", "UTC0", 1); tzset();
    struct tm date; memset(&date, 0, sizeof(date));
    date.tm_year = 126; date.tm_mon = 0; date.tm_mday = 1; date.tm_hour = 0; date.tm_sec = 2;
    time_t now = mktime(&date);
    IpcFixture ipc(1024);
    std::string chronological(1, '\0');
    const char* records[] = {"Dec 31 23:59:59 old\n", "Jan  1 00:00:02 equal\n", "Jan  1 00:00:01 out-of-order\n",
        "Jan  1 00:00:02 same-second\n\nJan  1 00:00:03 multiline\n", "invalid timestamp\n",
        "Jan  1 00:00:02.123 precise\n", "Jan"};
    for (unsigned long i = 0; i < sizeof(records) / sizeof(records[0]); ++i) {
        chronological += records[i]; chronological += '\0';
    }
    // Unusual long whitespace remains compatible with the original parser.
    chronological += "Jan" + std::string(80, ' ') + "1 00:00:02 unusual"; chronological += '\0';
    chronological += "Jan 1 00:00:" + std::string(80, '0') + "2 long-seconds"; chronological += '\0';
    chronological.append(ipc.capacity - chronological.size(), '\0');
    for (unsigned long cursor = 0; cursor < ipc.capacity; cursor += 31) checkQueries(&ipc, chronological, cursor, now);
    checkQueries(&ipc, std::string(ipc.capacity, '\0'), 1, now);
    std::string full = "overwritten-fragment"; full += '\0';
    while (full.size() + 24 < ipc.capacity) { full += "Jan  1 00:00:02 full"; full += '\0'; }
    full += std::string(ipc.capacity - full.size() - 1, 'x'); full += '\0';
    checkQueries(&ipc, full, ipc.capacity - 7, now);
    // Sparse large rings exercise block skipping and late-page materialization.
    IpcFixture sparse(512UL * 1024UL - 9);
    const std::string sparseMessage = "Jan  1 00:00:02 sparse";
    std::string sparseData(sparse.capacity - sparseMessage.size() - 1, '\0');
    sparseData += sparseMessage; sparseData += '\0';
    sparse.ring(sparseData, 77);
    SyslogReadStats stats; std::string json;
    assert(SyslogLogReader::readRequestForTest(sparse.key, false, 0, 1, 0, now, &json, &stats));
    assert(stats.materializedLines == 1 && stats.timestampConversions == 1 && stats.zeroBlocksSkipped > 1000);
    assert(!SyslogLogReader::readRequestForTest(sparse.key, false, 0, 501, 0, now, &json, &stats));
    assert(!SyslogLogReader::readRequestForTest(sparse.key, false, 0, 1, 0, now, 0, &stats));
    if (hadZone) setenv("TZ", savedZone.c_str(), 1); else unsetenv("TZ"); tzset();
}

}

void runSyslogLogReaderUnitTests() {
    queryTests();
    IpcFixture ipc;
    std::vector<std::string> lines;
    assert(SyslogLogReader::readLinesForTest(ipc.key, &lines) && lines.empty());
    const char messages[] = "Oct  8 10:00:00 first\n\0Oct  8 10:00:01 second\n\0";
    memcpy(ipc.bytes + 8, messages, sizeof(messages) - 1);
    ipc.header(64, sizeof(messages) - 1);
    assert(SyslogLogReader::readLinesForTest(ipc.key, &lines));
    assert(lines.size() == 2 && lines[0] == "Oct  8 10:00:00 first" && lines[1] == "Oct  8 10:00:01 second");
    // Full ring: discarded oldest fragment, a record crossing the physical end,
    // embedded LF, and the latest record ending just before the cursor.
    std::string content = "discarded";
    content += '\0'; content += "one\ntwo\n"; content += '\0';
    content += std::string(43, 'x'); content += '\n'; content += '\0';
    assert(content.size() == 64);
    ipc.ring(content, 60);
    assert(SyslogLogReader::readLinesForTest(ipc.key, &lines));
    assert(lines.size() == 3 && lines[0] == "one" && lines[1] == "two" && lines[2] == std::string(43, 'x'));
    assert(semctl(ipc.semaphore, 0, GETVAL) == 0);
    ipc.header(65, 0); assert(!SyslogLogReader::readLinesForTest(ipc.key, &lines));
    ipc.header(64, 64); assert(!SyslogLogReader::readLinesForTest(ipc.key, &lines));
    ipc.header(64, -1); assert(!SyslogLogReader::readLinesForTest(ipc.key, &lines));
    ipc.header(-1, 0); assert(!SyslogLogReader::readLinesForTest(ipc.key, &lines));
    ipc.reset(); ipc.bytes[72] = 'x'; assert(!SyslogLogReader::readLinesForTest(ipc.key, &lines));
    ipc.reset(); memset(ipc.bytes + 8, 'x', 64);
    // A nonempty ring without the latest record terminator is malformed.
    assert(!SyslogLogReader::readLinesForTest(ipc.key, &lines));
    ipc.reset(); ipc.bytes[8] = '\0'; ipc.bytes[17] = 'x'; ipc.header(64, 10);
    assert(!SyslogLogReader::readLinesForTest(ipc.key, &lines));
    ipc.reset(); SemValue writer; writer.value = 1;
    assert(semctl(ipc.semaphore, 1, SETVAL, writer) == 0);
    struct timeval before, after; gettimeofday(&before, 0);
    assert(!SyslogLogReader::readLinesForTest(ipc.key, &lines));
    gettimeofday(&after, 0);
    long elapsed = (after.tv_sec - before.tv_sec) * 1000000L + after.tv_usec - before.tv_usec;
    assert(elapsed >= 50000 && elapsed < 1000000);
    assert(semctl(ipc.semaphore, 0, GETVAL) == 0);
    ipc.reset(); assert(SyslogLogReader::readLinesForTest(ipc.key, &lines));
    assert(shmctl(ipc.segment, IPC_RMID, 0) == 0);
    assert(!SyslogLogReader::readLinesForTest(ipc.key, &lines));
}
