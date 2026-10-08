#include "syslog_log_reader.h"

#include <cassert>
#include <cstring>
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
    IpcFixture() : key(0), segment(-1), semaphore(-1), bytes(0) {
        for (unsigned int i = 0; i < 100; ++i) {
            key = 0x20000000L + static_cast<long>(getpid()) * 100 + i;
            segment = shmget(static_cast<key_t>(key), 73, IPC_CREAT | IPC_EXCL | 0600);
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
        memset(bytes, 0, 73);
        header(64, 0);
        unsigned short values[2] = {0, 0};
        SemValue argument; argument.values = values;
        assert(semctl(semaphore, 0, SETALL, argument) == 0);
    }
    void header(int32_t capacity, int32_t cursor) {
        memcpy(bytes, &capacity, 4); memcpy(bytes + 4, &cursor, 4);
    }
    void ring(const std::string& chronological, unsigned int cursor) {
        assert(chronological.size() == 64);
        for (unsigned int i = 0; i < 64; ++i) bytes[8 + (cursor + i) % 64] = chronological[i];
        header(64, cursor);
    }
};
}

void runSyslogLogReaderUnitTests() {
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
