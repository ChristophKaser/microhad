#include "sensor_history.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
std::string historyPath() {
    char name[] = "/tmp/microhad-history-test-XXXXXX";
    int fd = mkstemp(name);
    assert(fd >= 0);
    close(fd);
    unlink(name);
    return name;
}

void removeHistoryFiles(const std::string& path) {
    unlink(path.c_str());
    unlink((path + ".values").c_str());
    unlink((path + ".values.tmp").c_str());
}
}

void runSensorHistoryUnitTests() {
    fprintf(stderr, "unit: single source-neutral sensor history journal\n");
    const std::string path = historyPath();
    const std::string zigbeeId = "zigbee:0x00124b0000000001";
    const std::string matterId = "matter:0000000000000001:0000000000000002:0001";
    const std::string secondMatterId = "matter:0000000000000001:0000000000000002:0002";
    const std::string shellyId = "shelly:0011223344556677:temperature:2";
    std::string json, error;
    {
        FILE* legacy = fopen(path.c_str(), "w");
        assert(legacy != 0);
        assert(fputs("obsolete history", legacy) >= 0);
        fclose(legacy);

        SensorHistory history(path);
        assert(history.initialize(&error));
        struct stat legacyInfo;
        assert(stat(path.c_str(), &legacyInfo) != 0);
        struct stat valuesInfo;
        assert(stat((path + ".values").c_str(), &valuesInfo) == 0);
        assert(history.path() == path + ".values");

        assert(history.appendValue(zigbeeId, SensorHistory::Temperature, "-5.25", "celsius", 1708000000L));
        assert(history.appendValue(zigbeeId, SensorHistory::Humidity, "45.5", "percent", 1708000060L));
        assert(history.appendValue(matterId, SensorHistory::CarbonDioxide, "758.5", "ppb", 1708000000L));
        assert(history.appendValue(matterId, SensorHistory::Pm25, "12.75", "ug/m3", 1708000000L));
        assert(history.appendValue(matterId, SensorHistory::AirQuality, "moderate", "", 1708000001L));
        assert(history.appendValue(matterId, SensorHistory::Light, "1", "", 1708000002L));
        assert(history.appendValue(matterId, SensorHistory::Brightness, "50", "percent", 1708000003L));
        assert(history.appendValue(secondMatterId, SensorHistory::Temperature, "21.5", "celsius", 1708000000L));
        assert(history.appendValue(shellyId, SensorHistory::Temperature, "22.375", "celsius", 1708000000L));
        assert(history.appendValue(shellyId, SensorHistory::Contact, "1", "", 1708000001L));
        assert(history.appendValue(shellyId, SensorHistory::Power, "42.3", "watts", 1708000002L));
        assert(history.appendValue(zigbeeId, SensorHistory::Battery, "87", "percent", 1708000004L));
        assert(!history.appendValue(shellyId, SensorHistory::Power, "42.3", "kW", 1708000060L));
        assert(!history.appendValue(matterId, SensorHistory::CarbonDioxide, "758.5", "miles", 1708000060L));
        assert(history.readForDevice(zigbeeId, 0, &json, &error));
        assert(json.find("-5.25,\"celsius\"") != std::string::npos);
        assert(json.find("45.5,\"percent\"") != std::string::npos);
        assert(history.readForDevice(matterId, 0, &json, &error));
        assert(json.find("758.5,\"ppb\"") != std::string::npos);
        assert(json.find("12.75,\"ug/m3\"") != std::string::npos);
        assert(json.find("\"moderate\",\"\"") != std::string::npos);
        assert(json.find("21.5") == std::string::npos);
        assert(history.readForDevice(shellyId, 0, &json, &error));
        assert(json.find("22.375,\"celsius\"") != std::string::npos);
        assert(json.find(",3,1,\"\"") != std::string::npos);
        assert(json.find("42.3,\"watts\"") != std::string::npos);
        assert(history.readForDevice(zigbeeId, 0, &json, &error));
        assert(json.find(",6,87,\"percent\"") != std::string::npos);
        assert(history.readForDevice("0x00124b0000000001", 0, &json, &error));
        assert(json.find("-5.25,\"celsius\"") != std::string::npos);
    }
    {
        SensorHistory restarted(path);
        assert(restarted.initialize(&error));
        assert(restarted.readForDevice(zigbeeId, 0, &json, &error));
        assert(json.find("-5.25,\"celsius\"") != std::string::npos);
        assert(restarted.readForDevice(matterId, 0, &json, &error));
        assert(json.find("12.75,\"ug/m3\"") != std::string::npos);
    }
    removeHistoryFiles(path);

    const std::string boundedPath = historyPath();
    {
        SensorHistory bounded(boundedPath);
        char value[16];
        for (unsigned long i = 0; i < 8193; ++i) {
            snprintf(value, sizeof(value), "%lu", i % 10000);
            assert(bounded.appendValue(matterId, SensorHistory::CarbonDioxide, value, "ppm",
                                       1708000000L + static_cast<long>(i) * 60L));
        }
        struct stat boundedFile;
        assert(stat((boundedPath + ".values").c_str(), &boundedFile) == 0);
        assert(boundedFile.st_size <= 16 + 4097 * 128);
        assert(bounded.readForDevice(matterId, 0, &json, &error));
        assert(json.find(",7,0,\"ppm\"]") == std::string::npos);
        assert(json.find(",7,8192,\"ppm\"]") != std::string::npos);
    }
    removeHistoryFiles(boundedPath);
}
