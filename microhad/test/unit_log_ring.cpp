#include "log_ring.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <pthread.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
std::string temporaryPath() {
    char name[] = "/tmp/microhad-log-ring-test-XXXXXX";
    int fd = mkstemp(name);
    assert(fd >= 0);
    close(fd);
    unlink(name);
    return name;
}

void testWrapAndRestart() {
    const std::string path = temporaryPath();
    std::vector<std::string> written, retained;
    {
        LogRing ring(path);
        for (int i = 0; i < 5000; ++i) {
            std::ostringstream entry;
            entry << "short-" << i;
            written.push_back(entry.str());
            assert(ring.append(entry.str()));
        }
        assert(LogRing::readLines(path, &retained));
        assert(retained.size() == written.size());
        for (int i = 0; i < 3500; ++i) {
            std::ostringstream entry;
            entry << "long-" << i << std::string(640, 'x');
            written.push_back(entry.str());
            assert(ring.append(entry.str()));
        }
        assert(LogRing::readLines(path, &retained));
        assert(retained.size() > 1000 && retained.size() < written.size());
        for (unsigned long i = 0; i < retained.size(); ++i)
            assert(retained[i] == written[written.size() - retained.size() + i]);
        struct stat info;
        assert(stat(path.c_str(), &info) == 0 && info.st_size == LogRing::MaximumBytes);
    }
    {
        LogRing ring(path);
        assert(ring.append("after-restart \"quoted\"\nline"));
        std::string json;
        assert(LogRing::readJson(path, &json));
        assert(json.find("after-restart \\\"quoted\\\"\\nline") != std::string::npos);
        assert(LogRing::readLines(path, &retained));
        assert(retained.back() == "after-restart \"quoted\"\nline");
    }
    unlink(path.c_str());
}

void testLegacyMigrationAndInterruptedCommit() {
    const std::string path = temporaryPath();
    {
        std::ofstream output(path.c_str());
        output << "old-one\nold-two\n";
    }
    {
        LogRing ring(path);
        assert(ring.initialize());
        std::vector<std::string> lines;
        assert(LogRing::readLines(path, &lines));
        assert(lines.size() == 2 && lines[0] == "old-one" && lines[1] == "old-two");
        assert(ring.append("new-one"));
        assert(ring.append("new-two"));
    }
    /* The newest metadata copy can be torn by an interrupted write. */
    int fd = open(path.c_str(), O_WRONLY);
    assert(fd >= 0);
    const char bad = 0;
    assert(pwrite(fd, &bad, 1, 32) == 1);
    close(fd);
    std::vector<std::string> lines;
    assert(LogRing::readLines(path, &lines));
    assert(!lines.empty() && lines.back() == "new-one");
    {
        LogRing ring(path);
        assert(ring.append("recovered"));
    }
    assert(LogRing::readLines(path, &lines));
    assert(lines.back() == "recovered");
    unlink(path.c_str());
}

struct ConcurrentContext {
    std::string path;
    bool writerDone;
    pthread_mutex_t mutex;
    explicit ConcurrentContext(const std::string& value) : path(value), writerDone(false) { pthread_mutex_init(&mutex, 0); }
    ~ConcurrentContext() { pthread_mutex_destroy(&mutex); }
};
void* writeConcurrent(void* opaque) {
    ConcurrentContext* context = static_cast<ConcurrentContext*>(opaque);
    LogRing ring(context->path);
    for (int i = 0; i < 1800; ++i) {
        std::ostringstream line;
        line << "concurrent-" << i << std::string(600, 'q');
        assert(ring.append(line.str()));
    }
    pthread_mutex_lock(&context->mutex);
    context->writerDone = true;
    pthread_mutex_unlock(&context->mutex);
    return 0;
}
void testConcurrentReads() {
    const std::string path = temporaryPath();
    {
        LogRing ring(path);
        assert(ring.append("seed"));
    }
    ConcurrentContext context(path);
    pthread_t writer;
    assert(pthread_create(&writer, 0, writeConcurrent, &context) == 0);
    int successes = 0;
    for (int attempts = 0; attempts < 500; ++attempts) {
        std::string json;
        if (LogRing::readJson(path, &json)) {
            assert(json.find("{\"lines\":[") == 0 && json.substr(json.size() - 2) == "]}");
            ++successes;
        }
        pthread_mutex_lock(&context.mutex);
        bool done = context.writerDone;
        pthread_mutex_unlock(&context.mutex);
        if (done && successes > 0) break;
    }
    assert(pthread_join(writer, 0) == 0);
    assert(successes > 0);
    std::vector<std::string> lines;
    assert(LogRing::readLines(path, &lines));
    assert(lines.back().find("concurrent-1799") == 0);
    unlink(path.c_str());
}
}

void runLogRingUnitTests() {
    testWrapAndRestart();
    testLegacyMigrationAndInterruptedCommit();
    testConcurrentReads();
    printf("unit: bounded circular log, migration, and recovery\n");
}
