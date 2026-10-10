#include "sensor_history.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern void (*sensorHistorySnapshotHook)(void*);
extern void* sensorHistorySnapshotHookData;

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
unsigned long nextBefore(const std::string& json) {
    const std::string marker = "\"next_before\":";
    const std::string::size_type at = json.find(marker);
    assert(at != std::string::npos);
    return strtoul(json.c_str() + at + marker.size(), 0, 10);
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
        FILE* legacy = fopen(path.c_str(), "w"); assert(legacy != 0);
        assert(fputs("obsolete history", legacy) >= 0); fclose(legacy);
        FILE* flat = fopen((path + ".values").c_str(), "wb"); assert(flat != 0);
        const unsigned char oldHeader[16] = {'T', 'U', 'V', '1'};
        assert(fwrite(oldHeader, 1, sizeof(oldHeader), flat) == sizeof(oldHeader)); fclose(flat);
        SensorHistory history(path); assert(history.initialize(&error));
        struct stat oldInfo; assert(stat(path.c_str(), &oldInfo) != 0);
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
        assert(history.appendValue(matterId, SensorHistory::Light, "1", "", 1708000002L));
        assert(history.appendValue(matterId, SensorHistory::Brightness, "50", "percent", 1708000003L));
        assert(history.appendValue(secondMatterId, SensorHistory::Temperature, "21.5", "celsius", 1708000000L));
        assert(history.appendValue(shellyId, SensorHistory::Temperature, "22.375", "celsius", 1708000000L));
        assert(history.appendValue(shellyId, SensorHistory::Contact, "1", "", 1708000001L));
        assert(history.appendValue(shellyId, SensorHistory::Power, "42.3", "watts", 1708000002L));
        assert(history.appendValue(zigbeeId, SensorHistory::Battery, "87", "percent", 1708000004L));
        assert(!history.appendValue(shellyId, SensorHistory::Power, "42.3", "kW", 1708000060L));
        assert(!history.appendValue(matterId, SensorHistory::CarbonDioxide, "758.5", "miles", 1708000300L));

        struct stat info; assert(stat((path + ".values").c_str(), &info) == 0);
        assert(info.st_size >= 16 + 2048 && (info.st_size - 16) % 2048 == 0);
        FILE* file = fopen((path + ".values").c_str(), "rb"); assert(file != 0);
        unsigned char header[16]; assert(fread(header, 1, sizeof(header), file) == sizeof(header));
        assert(header[0] == 'T' && header[1] == 'Z' && header[2] == 'H' && header[3] == '3');
        assert(header[6] == 85 && header[7] == 13 && header[9] == 151 && header[11] == 0xff);
        unsigned char pageHeader[85]; assert(fread(pageHeader, 1, sizeof(pageHeader), file) == sizeof(pageHeader));
        for (int i = 71; i <= 83; ++i) assert(pageHeader[i] == 0);
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
        assert(json.find(",11,50,\"percent\"") != std::string::npos);
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

    const std::string boundedPath = historyPath();
    {
        SensorHistory bounded(boundedPath); char value[16];
        for (unsigned long i = 0; i < 154474; ++i) {
            snprintf(value, sizeof(value), "%lu", i);
            assert(bounded.appendValue(matterId, SensorHistory::CarbonDioxide, value, "ppm",
                                       1708000000L + static_cast<long>(i) * 60L));
        }
        struct stat boundedFile; assert(stat((boundedPath + ".values").c_str(), &boundedFile) == 0);
        assert(boundedFile.st_size == 16 + 1023 * 2048);
        SensorHistory::ReadStats latestStats;
        assert(bounded.readForDevice(matterId, 0, &json, &error, &latestStats));
        assert(latestStats.pagesRead <= 15 && latestStats.recordsChecked <= 2300);
        const unsigned long cursor = nextBefore(json);
        assert(cursor != 0);
        SensorHistory::ReadStats olderStats;
        std::string older;
        assert(bounded.readForDevice(matterId, cursor, &older, &error, &olderStats));
        assert(olderStats.pagesRead <= 16 && olderStats.recordsChecked <= 2400);
        assert(nextBefore(older) < cursor);
        assert(json.find(",7,154473,\"ppm\"]") != std::string::npos);
        assert(bounded.readForDevice(matterId, 152, &json, &error));
        assert(json == "{\"samples\":[],\"next_before\":0}");
    }
    {
        SensorHistory restarted(boundedPath);
        assert(restarted.initialize(&error));
        assert(restarted.appendValue(matterId, SensorHistory::CarbonDioxide, "154474", "ppm", 1717268440L));
        assert(restarted.readForDevice(matterId, 303, &json, &error));
        assert(json.find("[152,") != std::string::npos);
        assert(json.find("[302,") != std::string::npos);
    }
    removeHistoryFiles(boundedPath);

    const std::string collisionPath = historyPath();
    const std::string collisionId = "zigbee:0x00124b0000000463";
    {
        SensorHistory history(collisionPath);
        for (unsigned int i = 0; i < 151; ++i)
            assert(history.appendValue(zigbeeId, SensorHistory::Contact, i % 2 ? "1" : "0", "", 1708000000L + i));
        char value[16];
        for (unsigned long i = 0; i < 1022 * 151; ++i) {
            snprintf(value, sizeof(value), "%lu", i);
            assert(history.appendValue(matterId, SensorHistory::CarbonDioxide, value, "ppm", 1708000200L + i * 60));
        }
        EvictionContext context; context.history = &history; context.id = collisionId;
        sensorHistorySnapshotHook = evictAfterSnapshot;
        sensorHistorySnapshotHookData = &context;
        assert(history.readForDevice(zigbeeId, 0, &json, &error));
        sensorHistorySnapshotHook = 0;
        sensorHistorySnapshotHookData = 0;
        assert(json == "{\"samples\":[],\"next_before\":0}");
    }
    {
        SensorHistory restarted(collisionPath);
        assert(restarted.initialize(&error));
        assert(restarted.readForDevice(collisionId, 0, &json, &error));
        assert(json == "{\"samples\":[[154474,1718000000,3,1,\"\"]],\"next_before\":0}");
    }
    removeHistoryFiles(collisionPath);
}
