#include "sensor_history.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

extern void (*sensorHistorySnapshotHook)(void*);
extern void* sensorHistorySnapshotHookData;
extern void (*sensorHistoryPersistenceCopyHook)(void*);
extern void* sensorHistoryPersistenceCopyHookData;
extern bool sensorHistoryFailPersistenceSync;

namespace {
std::string historyPath() {
    char name[] = "/tmp/microhad-history-test-XXXXXX";
    int fd = mkstemp(name); assert(fd >= 0); close(fd); unlink(name); return name;
}
void removeHistoryFiles(const std::string& path) {
    unlink(path.c_str()); unlink((path + ".values").c_str()); unlink((path + ".values.tmp").c_str());
}
struct EvictionContext {
    SensorHistory* history;
    std::string id;
};
void evictAfterSnapshot(void* data) {
    EvictionContext* context = static_cast<EvictionContext*>(data);
    assert(context->history->appendValue(context->id, SensorHistory::Contact, "1", "", 1718000000L));
}
uint64_t nextBefore(const std::string& json) {
    const std::string marker = "\"next_before\":";
    const std::string::size_type at = json.find(marker);
    assert(at != std::string::npos);
    return strtoull(json.c_str() + at + marker.size(), 0, 10);
}
}

void runSensorHistoryUnitTests() {
    fprintf(stderr, "unit: paged sensor history v3\n");
    const std::string path = historyPath();
    const std::string zigbeeId = "zigbee:0x00124b0000000001";
    const std::string matterId = "matter:0000000000000001:0000000000000002:0001";
    const std::string secondMatterId = "matter:0000000000000001:0000000000000002:0002";
    const std::string shellyId = "shelly:0011223344556677:temperature:2";
    std::string json, error;
    {
        SensorHistory history(path); assert(history.initialize(&error));
        assert(history.path() == path + ".values");
        assert(history.appendValue(zigbeeId, SensorHistory::Temperature, "-5.25", "celsius", 1708000000L));
        assert(history.appendValue(zigbeeId, SensorHistory::Humidity, "45.5", "percent", 1708000060L));
        assert(history.appendValue(matterId, SensorHistory::CarbonDioxide, "758.5", "ppb", 1708000000L));
        assert(history.appendValue(matterId, SensorHistory::CarbonDioxide, "0.7585", "ppm", 1708000060L));
        assert(history.appendValue(matterId, SensorHistory::CarbonDioxide, "3", "mg/m3", 1708000120L));
        assert(history.appendValue(matterId, SensorHistory::CarbonDioxide, "2", "particles/ft3", 1708000180L));
        assert(history.appendValue(matterId, SensorHistory::CarbonDioxide, "4", "Bq/m3", 1708000240L));
        assert(history.appendValue(matterId, SensorHistory::Pm25, "12.75", "ug/m3", 1708000000L));
        assert(history.appendValue(matterId, SensorHistory::Pm25, "12750", "ng/m3", 1708000060L));
        assert(history.appendValue(matterId, SensorHistory::Pm25, "900", "ppb", 1708000120L));
        assert(history.appendValue(matterId, SensorHistory::Pm25, "2", "particles/ft3", 1708000180L));
        assert(history.appendValue(matterId, SensorHistory::AirQuality, "moderate", "", 1708000001L));
        assert(!history.appendValue(matterId, SensorHistory::Light, "1", "", 1708000002L));
        assert(!history.appendValue(matterId, SensorHistory::Brightness, "50", "percent", 1708000003L));
        assert(history.appendValue(secondMatterId, SensorHistory::Temperature, "21.5", "celsius", 1708000000L));
        assert(history.appendValue(shellyId, SensorHistory::Temperature, "22.375", "celsius", 1708000000L));
        assert(history.appendValue(shellyId, SensorHistory::Contact, "1", "", 1708000001L));
        assert(history.appendValue(shellyId, SensorHistory::Power, "42.3", "watts", 1708000002L));
        assert(history.appendValue(zigbeeId, SensorHistory::Battery, "87", "percent", 1708000004L));
        assert(!history.appendValue(shellyId, SensorHistory::Power, "42.3", "kW", 1708000060L));
        assert(!history.appendValue(matterId, SensorHistory::CarbonDioxide, "758.5", "miles", 1708000300L));

        assert(!history.appendValue("fritz:001122334455:presence", SensorHistory::Occupancy, "1", "", 1708000000L));
        assert(history.readForDevice("fritz:001122334455:presence", 0, &json, &error));
        assert(json == "{\"samples\":[],\"next_before\":0}");

        struct stat info; assert(stat((path + ".values").c_str(), &info) == 0);
        assert(info.st_size >= 16 + 2048 && (info.st_size - 16) % 2048 == 0);
        FILE* file = fopen((path + ".values").c_str(), "rb"); assert(file != 0);
        unsigned char header[16]; assert(fread(header, 1, sizeof(header), file) == sizeof(header));
        assert(header[0] == 'T' && header[1] == 'Z' && header[2] == 'H' && header[3] == '3');
        assert(header[6] == 86 && header[7] == 9 && header[9] == 218 && header[11] == 0xff);
        unsigned char pageHeader[86]; assert(fread(pageHeader, 1, sizeof(pageHeader), file) == sizeof(pageHeader));
        for (int i = 71; i <= 84; ++i) assert(pageHeader[i] == 0);
        fclose(file);

        assert(history.readForDevice(zigbeeId, 0, &json, &error));
        assert(json.find("-5.25,\"celsius\"") != std::string::npos);
        assert(json.find("45.5,\"percent\"") != std::string::npos);
        assert(history.readForDevice(matterId, 0, &json, &error));
        assert(json.find(",7,0.75849998,\"ppm\"]") != std::string::npos);
        assert(json.find(",12,3000,\"ug/m3\"]") != std::string::npos);
        assert(json.find(",13,70.6293335,\"particles/m3\"]") != std::string::npos);
        assert(json.find(",14,4,\"Bq/m3\"]") != std::string::npos);
        assert(json.find(",8,12.75,\"ug/m3\"]") != std::string::npos);
        assert(json.find(",16,0.899999976,\"ppm\"]") != std::string::npos);
        assert(json.find(",17,70.6293335,\"particles/m3\"]") != std::string::npos);
        assert(json.find("\"moderate\",\"\"") != std::string::npos);
        assert(json.find(",11,50,\"percent\"") == std::string::npos);
        assert(json.find("21.5") == std::string::npos);
        assert(history.readForDevice(shellyId, 0, &json, &error));
        assert(json.find("22.375,\"celsius\"") != std::string::npos);
        assert(json.find(",3,1,\"\"") != std::string::npos);
        assert(json.find("42.2999992,\"watts\"") != std::string::npos);
        assert(history.readForDevice(zigbeeId, 0, &json, &error));
        assert(json.find(",6,87,\"percent\"") != std::string::npos);
        assert(history.readForDevice("0x00124b0000000001", 0, &json, &error));
        assert(json.find("-5.25,\"celsius\"") != std::string::npos);
    }
    {
        SensorHistory restarted(path); assert(restarted.initialize(&error));
        assert(restarted.readForDevice(zigbeeId, 0, &json, &error));
        assert(json.find("-5.25,\"celsius\"") != std::string::npos);
        assert(restarted.readForDevice(matterId, 0, &json, &error));
        assert(json.find(",7,0.75849998,\"ppm\"]") != std::string::npos);
    }
    removeHistoryFiles(path);

    // Daily reports retain full timestamps and fill one page over many months.
    const std::string dailyPath = historyPath();
    {
        SensorHistory daily(dailyPath);
        for (unsigned int i = 0; i < 218; ++i)
            assert(daily.appendValue(zigbeeId, SensorHistory::Contact, i % 2 ? "1" : "0", "",
                                     1708000000L + i * 86400L));
        struct stat info; assert(stat((dailyPath + ".values").c_str(), &info) == 0);
        assert(info.st_size == 16 + 2048);
    }
    {
        SensorHistory restarted(dailyPath);
        assert(restarted.readForDevice(zigbeeId, 0, &json, &error));
        assert(json.find(",1726748800,3,1,\"\"]") != std::string::npos);
    }
    removeHistoryFiles(dailyPath);

    // Same-second transitions have distinct derived cursors across pagination.
    const std::string tiedPath = historyPath();
    {
        SensorHistory tied(tiedPath);
        for (unsigned int i = 0; i < 2100; ++i)
            assert(tied.appendValue(zigbeeId, SensorHistory::Contact, i % 2 ? "1" : "0", "", 1708000000L));
        assert(tied.readForDevice(zigbeeId, 0, &json, &error));
        const uint64_t cursor = nextBefore(json);
        assert(cursor > 0xffffffffUL);
        assert(tied.readForDevice(zigbeeId, cursor, &json, &error));
        unsigned int count = 0;
        for (std::string::size_type i = 0; i < json.size(); ++i) if (json[i] == '[') ++count;
        assert(count == 101 && nextBefore(json) == 0);
    }
    removeHistoryFiles(tiedPath);

    const std::string boundedPath = historyPath();
    {
        SensorHistory bounded(boundedPath); char value[16];
        for (unsigned long i = 0; i < 223015; ++i) {
            snprintf(value, sizeof(value), "%lu", i);
            assert(bounded.appendValue(matterId, SensorHistory::CarbonDioxide, value, "ppm",
                                       1708000000L + static_cast<long>(i) * 60L));
        }
        struct stat boundedFile; assert(stat((boundedPath + ".values").c_str(), &boundedFile) == 0);
        assert(boundedFile.st_size == 16 + 1023 * 2048);
        SensorHistory::ReadStats latestStats;
        assert(bounded.readForDevice(matterId, 0, &json, &error, &latestStats));
        assert(latestStats.pagesRead <= 11 && latestStats.recordsChecked <= 2400);
        const uint64_t cursor = nextBefore(json);
        assert(cursor != 0);
        SensorHistory::ReadStats olderStats;
        std::string older;
        assert(bounded.readForDevice(matterId, cursor, &older, &error, &olderStats));
        assert(olderStats.pagesRead <= 12 && olderStats.recordsChecked <= 2600);
        assert(nextBefore(older) < cursor);
        assert(json.find(",7,223014,\"ppm\"]") != std::string::npos);
        assert(bounded.readForDevice(matterId, static_cast<uint64_t>(1708000000L + 218 * 60) << 18, &json, &error));
        assert(json == "{\"samples\":[],\"next_before\":0}");
    }
    {
        SensorHistory restarted(boundedPath);
        assert(restarted.initialize(&error));
        assert(restarted.appendValue(matterId, SensorHistory::CarbonDioxide, "223015", "ppm", 1721380900L));
        assert(restarted.readForDevice(matterId, static_cast<uint64_t>(1708000000L + 436 * 60) << 18, &json, &error));
        assert(json.find(",7,218,\"ppm\"]") != std::string::npos);
        assert(json.find(",7,435,\"ppm\"]") != std::string::npos);
    }
    removeHistoryFiles(boundedPath);

    const std::string collisionPath = historyPath();
    const std::string collisionFlash = historyPath();
    // Its identity at allocation generation 1024 collides with the old owner
    // at generation 1 under CRC-8, so checksums alone cannot clear stale slots.
    const std::string collisionId = "zigbee:0x00124b00000000ac";
    {
        SensorHistory history(collisionPath, collisionFlash); history.useManualPersistenceForTests();
        for (unsigned int i = 0; i < 218; ++i)
            assert(history.appendValue(zigbeeId, SensorHistory::Contact, i % 2 ? "1" : "0", "", 1708000000L + i));
        assert(history.persistForTests(0, &error));
        assert(history.persistenceWritesForTests() == 1);
        char value[16];
        for (unsigned long i = 0; i < 1022 * 218; ++i) {
            snprintf(value, sizeof(value), "%lu", i);
            assert(history.appendValue(matterId, SensorHistory::CarbonDioxide, value, "ppm", 1708000200L + i * 60));
        }
        assert(history.persistForTests(0, &error));
        const unsigned long writtenBeforeEviction = history.persistenceWritesForTests();
        assert(writtenBeforeEviction == 255);
        EvictionContext context; context.history = &history; context.id = collisionId;
        sensorHistorySnapshotHook = evictAfterSnapshot;
        sensorHistorySnapshotHookData = &context;
        assert(history.readForDevice(zigbeeId, 0, &json, &error));
        sensorHistorySnapshotHook = 0;
        sensorHistorySnapshotHookData = 0;
        assert(json == "{\"samples\":[],\"next_before\":0}");
        assert(history.persistForTests(0, &error));
        assert(history.persistenceWritesForTests() == writtenBeforeEviction);
        FILE* resident = fopen(collisionFlash.c_str(), "rb"); assert(resident != 0);
        assert(fseek(resident, 16, SEEK_SET) == 0);
        unsigned char residentHeader[86];
        assert(fread(residentHeader, 1, sizeof(residentHeader), resident) == sizeof(residentHeader)); fclose(resident);
        assert(std::string(reinterpret_cast<char*>(residentHeader + 7), residentHeader[0]) == zigbeeId);
    }
    {
        SensorHistory restarted(collisionPath);
        assert(restarted.initialize(&error));
        assert(restarted.readForDevice(collisionId, 0, &json, &error));
        std::ostringstream expected;
        expected << "{\"samples\":[[" << (static_cast<uint64_t>(1718000000) << 18)
                 << ",1718000000,3,1,\"\"]],\"next_before\":0}";
        assert(json == expected.str());
    }
    removeHistoryFiles(collisionPath);
    {
        SensorHistory history(collisionPath, collisionFlash); history.useManualPersistenceForTests();
        assert(history.readForDevice(zigbeeId, 0, &json, &error));
        assert(json.find(",1708000000,3,0,") != std::string::npos);
    }
    removeHistoryFiles(collisionPath); unlink(collisionFlash.c_str());
    // Flash pages are stable residents, not a compacted copy of the RAM pool.
    const std::string persistentPath = historyPath();
    const std::string flashPath = historyPath();
    {
        SensorHistory history(persistentPath, flashPath); history.useManualPersistenceForTests();
        assert(history.appendValue(zigbeeId, SensorHistory::Contact, "0", "", 1708000000L));
        assert(history.persistForTests(14399, &error));
        assert(access(flashPath.c_str(), F_OK) != 0);
        assert(history.persistForTests(14400, &error));
        assert(history.persistenceWritesForTests() == 1);
        assert(history.persistForTests(28800, &error));
        assert(history.persistenceWritesForTests() == 1);
        assert(history.appendValue(zigbeeId, SensorHistory::Contact, "1", "", 1708086400L));
        assert(history.persistForTests(43199, &error));
        assert(history.persistenceWritesForTests() == 1);
        assert(history.persistForTests(43200, &error));
        assert(history.persistenceWritesForTests() == 2);
        struct stat info; assert(stat(flashPath.c_str(), &info) == 0);
        assert(info.st_size == 16 + 2048); // Sparse update reused slot zero.
        assert(history.appendValue(zigbeeId, SensorHistory::Contact, "0", "", 1708172800L));
    } // No shutdown flush: RAM contains three samples, flash only two.
    {
        SensorHistory restarted(persistentPath, flashPath); restarted.useManualPersistenceForTests();
        assert(restarted.readForDevice(zigbeeId, 0, &json, &error));
        assert(json.find(",1708172800,3,0,") != std::string::npos);
    }
    removeHistoryFiles(persistentPath);
    {
        SensorHistory restored(persistentPath, flashPath); restored.useManualPersistenceForTests();
        assert(restored.readForDevice(zigbeeId, 0, &json, &error));
        assert(json.find(",1708086400,3,1,") != std::string::npos);
        assert(json.find(",1708172800,3,0,") == std::string::npos);
        assert(restored.persistForTests(14400, &error));
        assert(restored.persistenceWritesForTests() == 0);
        // Filling the restored sparse page writes it immediately in its original slot.
        for (unsigned int i = 2; i < 218; ++i)
            assert(restored.appendValue(zigbeeId, SensorHistory::Contact, i % 2 ? "1" : "0", "", 1708172800L + i));
        assert(restored.persistForTests(14401, &error));
        assert(restored.persistenceWritesForTests() == 1);
        struct stat info; assert(stat(flashPath.c_str(), &info) == 0);
        assert(info.st_size == 16 + 2048);
    }
    removeHistoryFiles(persistentPath); unlink(flashPath.c_str());

    const std::string retentionPath = historyPath();
    const std::string retentionFlash = historyPath();
    {
        SensorHistory history(retentionPath, retentionFlash); history.useManualPersistenceForTests();
        unsigned char stable[2048];
        for (unsigned int device = 0; device < 256; ++device) {
            std::ostringstream id; id << "test:device:" << device;
            for (unsigned int i = 0; i < 218; ++i)
                assert(history.appendValue(id.str(), SensorHistory::Contact, i % 2 ? "1" : "0", "", 1708000000L + i));
            assert(history.persistForTests(0, &error));
            if (device == 1) {
                FILE* f = fopen(retentionFlash.c_str(), "rb"); assert(f != 0);
                assert(fseek(f, 16 + 2048, SEEK_SET) == 0);
                assert(fread(stable, 1, sizeof(stable), f) == sizeof(stable)); fclose(f);
            }
        }
        struct stat info; assert(stat(retentionFlash.c_str(), &info) == 0);
        assert(info.st_size == 16 + 255 * 2048 && info.st_size < 512 * 1024);
        assert(history.persistenceWritesForTests() == 256);
        unsigned char page[2048]; FILE* f = fopen(retentionFlash.c_str(), "rb"); assert(f != 0);
        assert(fseek(f, 16 + 2048, SEEK_SET) == 0);
        assert(fread(page, 1, sizeof(page), f) == sizeof(page));
        assert(memcmp(page, stable, sizeof(page)) == 0); // Resident never moved or rewritten.
        assert(fseek(f, 16, SEEK_SET) == 0);
        assert(fread(page, 1, sizeof(page), f) == sizeof(page)); fclose(f);
        assert(std::string(reinterpret_cast<char*>(page + 7), page[0]) == "test:device:255");
        assert(history.persistForTests(14400, &error));
        assert(history.persistenceWritesForTests() == 256);
    }
    // Compact RAM restore must not compact or otherwise rewrite flash slots.
    removeHistoryFiles(retentionPath);
    {
        SensorHistory history(retentionPath, retentionFlash); history.useManualPersistenceForTests();
        assert(history.readForDevice("test:device:0", 0, &json, &error));
        assert(json == "{\"samples\":[],\"next_before\":0}");
        assert(history.readForDevice("test:device:1", 0, &json, &error));
        assert(json.find(",3,1,") != std::string::npos);
        assert(history.persistForTests(14400, &error));
        assert(history.persistenceWritesForTests() == 0);
    }
    removeHistoryFiles(retentionPath);

    // A truncated final page is skipped while earlier committed pages survive.
    assert(truncate(retentionFlash.c_str(), 16 + 255 * 2048 - 1) == 0);
    {
        SensorHistory history(retentionPath, retentionFlash); history.useManualPersistenceForTests();
        assert(history.readForDevice("test:device:1", 0, &json, &error));
        assert(json.find(",3,1,") != std::string::npos);
        assert(history.readForDevice("test:device:254", 0, &json, &error));
        assert(json == "{\"samples\":[],\"next_before\":0}");
    }
    removeHistoryFiles(retentionPath); unlink(retentionFlash.c_str());

    // Unreadable/corrupt flash must not prevent live RAM history or be overwritten.
    const std::string badRam = historyPath(), badFlash = historyPath();
    {
        FILE* f = fopen(badFlash.c_str(), "wb"); assert(f != 0);
        assert(fputs("damaged flash", f) >= 0); fclose(f);
        SensorHistory history(badRam, badFlash); history.useManualPersistenceForTests();
        assert(history.initialize(&error));
        assert(!history.takePersistenceError().empty());
        assert(history.appendValue(zigbeeId, SensorHistory::Contact, "1", "", 1708000000L));
        assert(history.persistForTests(14400, &error));
        struct stat info; assert(stat(badFlash.c_str(), &info) == 0 && info.st_size == 13);
    }
    removeHistoryFiles(badRam); unlink(badFlash.c_str());

    const std::string retryRam = historyPath(), retryDirectory = historyPath();
    {
        SensorHistory history(retryRam, retryDirectory + "/history.values"); history.useManualPersistenceForTests();
        assert(history.appendValue(zigbeeId, SensorHistory::Contact, "1", "", 1708000000L));
        assert(!history.persistForTests(14400, &error) && !error.empty());
        assert(mkdir(retryDirectory.c_str(), 0700) == 0);
        assert(history.persistForTests(14459, &error));
        assert(history.persistenceWritesForTests() == 0);
        assert(history.persistForTests(14460, &error));
        assert(history.persistenceWritesForTests() == 1);
    }
    removeHistoryFiles(retryRam); unlink((retryDirectory + "/history.values").c_str()); rmdir(retryDirectory.c_str());

    // A failed sync cannot move a page already written to a slot on retry.
    const std::string syncRam = historyPath(), syncFlash = historyPath();
    {
        SensorHistory history(syncRam, syncFlash); history.useManualPersistenceForTests();
        for (unsigned int i = 0; i < 218; ++i)
            assert(history.appendValue(zigbeeId, SensorHistory::Contact, i % 2 ? "1" : "0", "", 1708000000L + i));
        sensorHistoryFailPersistenceSync = true;
        assert(!history.persistForTests(0, &error));
        sensorHistoryFailPersistenceSync = false;
        assert(history.persistenceWritesForTests() == 0);
        for (unsigned int i = 0; i < 218; ++i)
            assert(history.appendValue(matterId, SensorHistory::Contact, i % 2 ? "1" : "0", "", 1708000000L + i));
        assert(history.persistForTests(60, &error));
        assert(history.persistenceWritesForTests() == 2);
        FILE* f = fopen(syncFlash.c_str(), "rb"); assert(f != 0);
        assert(fseek(f, 16, SEEK_SET) == 0);
        unsigned char header[86]; assert(fread(header, 1, sizeof(header), f) == sizeof(header)); fclose(f);
        assert(std::string(reinterpret_cast<char*>(header + 7), header[0]) == zigbeeId);
        assert(history.persistForTests(14400, &error));
        assert(history.persistenceWritesForTests() == 2);
    }
    removeHistoryFiles(syncRam); unlink(syncFlash.c_str());

    // An append after the worker's page copy stays dirty until the next sparse sweep.
    const std::string concurrentRam = historyPath(), concurrentFlash = historyPath();
    {
        SensorHistory history(concurrentRam, concurrentFlash); history.useManualPersistenceForTests();
        assert(history.appendValue(zigbeeId, SensorHistory::Contact, "0", "", 1708000000L));
        EvictionContext context; context.history = &history; context.id = zigbeeId;
        sensorHistoryPersistenceCopyHook = evictAfterSnapshot;
        sensorHistoryPersistenceCopyHookData = &context;
        assert(history.persistForTests(14400, &error));
        sensorHistoryPersistenceCopyHook = 0; sensorHistoryPersistenceCopyHookData = 0;
        assert(history.persistenceWritesForTests() == 1);
        assert(history.persistForTests(14401, &error));
        assert(history.persistenceWritesForTests() == 1);
        assert(history.persistForTests(28800, &error));
        assert(history.persistenceWritesForTests() == 2);
    }
    removeHistoryFiles(concurrentRam);
    {
        SensorHistory history(concurrentRam, concurrentFlash); history.useManualPersistenceForTests();
        assert(history.readForDevice(zigbeeId, 0, &json, &error));
        assert(json.find(",1718000000,3,1,") != std::string::npos);
    }
    removeHistoryFiles(concurrentRam); unlink(concurrentFlash.c_str());

    // Exercise the actual background worker and its shutdown wakeup.
    const std::string workerRam = historyPath(), workerFlash = historyPath();
    {
        SensorHistory history(workerRam, workerFlash);
        for (unsigned int i = 0; i < 218; ++i)
            assert(history.appendValue(zigbeeId, SensorHistory::Contact, i % 2 ? "1" : "0", "", 1708000000L + i));
        for (unsigned int wait = 0; wait < 500 && history.persistenceWritesForTests() == 0; ++wait) usleep(10000);
        assert(history.persistenceWritesForTests() == 1);
        assert(history.takePersistenceError().empty());
    }
    removeHistoryFiles(workerRam); unlink(workerFlash.c_str());

}
