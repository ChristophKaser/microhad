#include "gateway.h"
#include "sensor_history.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
const char* sensor = "0x00124b0000000042";
const char* other = "0x00124b0000000043";
const char* filler = "0x00124b0000000044";
const char* concurrent = "0x00124b0000000045";

std::string historyPath() {
    char name[] = "/tmp/microhad-history-test-XXXXXX";
    int fd = mkstemp(name);
    assert(fd >= 0);
    close(fd);
    unlink(name);
    return name;
}

unsigned long nextBefore(const std::string& json) {
    std::string::size_type at = json.find("\"next_before\":");
    assert(at != std::string::npos);
    return strtoul(json.c_str() + at + 14, 0, 10);
}
void* writeConcurrent(void* opaque) {
    SensorHistory history(*static_cast<std::string*>(opaque));
    for (int i = 0; i < 200; ++i)
        assert(history.append(concurrent, SensorHistory::Humidity, 4000 + i, 1700500000L + i * 60L));
    return 0;
}
}

void runSensorHistoryUnitTests() {
    fprintf(stderr, "unit: bounded binary sensor history and recovery\n");
    std::string path = historyPath();
    std::string json, error;
    {
        SensorHistory history(path);
        assert(history.append(sensor, SensorHistory::Temperature, -2150, 1700000000));
        assert(history.append(sensor, SensorHistory::Temperature, -2100, 1700000010));
        assert(history.append(sensor, SensorHistory::Humidity, 5030, 1700000010));
        assert(history.append(sensor, SensorHistory::Contact, 1, 1700000011));
        assert(history.append(sensor, SensorHistory::Contact, 0, 1700000012));
    }
    assert(SensorHistory::readJson(path, sensor, 0, &json, &error));
    assert(json.find(",1,-2150]") != std::string::npos);
    assert(json.find("-2100") == std::string::npos); /* same minute */
    assert(json.find(",2,5030]") != std::string::npos);
    assert(json.find(",3,1]") != std::string::npos && json.find(",3,0]") != std::string::npos);
    {
        SensorHistory restarted(path);
        assert(restarted.append(sensor, SensorHistory::Temperature, -2000, 1700000020));
        assert(restarted.append(sensor, SensorHistory::Temperature, -1900, 1700000060));
    }
    assert(SensorHistory::readJson(path, sensor, 0, &json, &error));
    assert(json.find("-2000") == std::string::npos && json.find("-1900") != std::string::npos);

    {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "", 0, path);
        std::vector<unsigned char> temperature;
        temperature.push_back(0x2c); temperature.push_back(0x09); /* 23.48 C */
        assert(gateway.applyAttributeReport(other, 0x0402, 0, temperature));
        std::vector<unsigned char> humidity;
        humidity.push_back(0xa6); humidity.push_back(0x13); /* 50.30% */
        assert(gateway.applyAttributeReport(other, 0x0405, 0, humidity));
        std::vector<unsigned char> zone(2, 0);
        assert(gateway.applyIasZoneStatus(other, zone));
        zone[0] = 1;
        assert(gateway.applyIasZoneStatus(other, zone));
    }
    assert(SensorHistory::readJson(path, other, 0, &json, &error));
    assert(json.find(",1,2348]") != std::string::npos);
    assert(json.find(",2,5030]") != std::string::npos);
    assert(json.find(",3,0]") != std::string::npos && json.find(",3,1]") != std::string::npos);

    pthread_t writer;
    assert(pthread_create(&writer, 0, writeConcurrent, &path) == 0);
    for (int i = 0; i < 50; ++i) {
        assert(SensorHistory::readJson(path, concurrent, 0, &json, &error));
        assert(json.find("\"samples\":[") == 1);
    }
    assert(pthread_join(writer, 0) == 0);

    {
        SensorHistory history(path);
        for (int i = 0; i < 10005; ++i)
            assert(history.append(sensor, SensorHistory::Temperature, i % 100, 1700100000L + i * 60L));
    }
    unsigned long before = 0;
    for (int page = 0; page < 6; ++page) {
        assert(SensorHistory::readJson(path, sensor, before, &json, &error));
        before = nextBefore(json);
        if (page < 5) assert(before != 0); else assert(before == 0);
    }
    assert(json.find(",1700100000,1,0]") != std::string::npos); /* oldest retained sample */
    {
        SensorHistory history(path);
        for (int i = 0; i < 104860; ++i)
            assert(history.append(filler, SensorHistory::Temperature, i % 100, 1701000000L + i * 60L));
    }
    struct stat info;
    assert(stat(path.c_str(), &info) == 0 && info.st_size <= 2 * 1024 * 1024);
    assert(SensorHistory::readJson(path, sensor, 0, &json, &error));
    assert(json == "{\"samples\":[],\"next_before\":0}");
    unlink(path.c_str());

    path = historyPath();
    {
        SensorHistory history(path);
        assert(history.append(sensor, SensorHistory::Humidity, 4300, 1700000000));
    }
    int fd = open(path.c_str(), O_WRONLY);
    assert(fd >= 0);
    const char bad = 0;
    assert(pwrite(fd, &bad, 1, 16 + 18) == 1);
    close(fd);
    assert(SensorHistory::readJson(path, sensor, 0, &json, &error));
    assert(json == "{\"samples\":[],\"next_before\":0}");
    unlink(path.c_str());

    path = historyPath();
    {
        SensorHistory history(path);
        assert(history.append(sensor, SensorHistory::Power, 35000, 1702000000L));
        assert(history.append(sensor, SensorHistory::Power, 35000, 1702000060L));
        assert(history.append(sensor, SensorHistory::Temperature, 2100, 1702000000L));
        assert(history.append(sensor, SensorHistory::Temperature, 2100, 1702000060L));
        assert(history.append(sensor, SensorHistory::Occupancy, 1, 1702000061L));
        assert(history.append(sensor, SensorHistory::Battery, 87, 1702000062L));
    }
    {
        SensorHistory restarted(path);
        assert(restarted.append(sensor, SensorHistory::Power, 35000, 1702000120L));
        assert(restarted.append(sensor, SensorHistory::Power, 35001, 1702000120L));
        assert(restarted.append(sensor, SensorHistory::Temperature, 2100, 1702000120L));
    }
    assert(SensorHistory::readJson(path, sensor, 0, &json, &error));
    assert(json.find(",4,35000]") != std::string::npos);
    assert(json.find(",4,35001]") != std::string::npos);
    assert(json.find(",1,2100]") != std::string::npos);
    assert(json.find(",5,1]") != std::string::npos);
    assert(json.find(",6,87]") != std::string::npos);
    assert(json.find("1702000060") == std::string::npos);
    unlink(path.c_str());
}
