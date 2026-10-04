#include "gateway.h"
#include "sensor_history.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
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
void removeHistoryFiles(const std::string& path) {
    unlink(path.c_str());
    unlink((path + ".values").c_str());
    unlink((path + ".values.tmp").c_str());
}

unsigned long nextBefore(const std::string& json) {
    std::string::size_type at = json.find("\"next_before\":");
    assert(at != std::string::npos);
    return strtoul(json.c_str() + at + 14, 0, 10);
}
void* writeConcurrent(void* opaque) {
    SensorHistory* history = static_cast<SensorHistory*>(opaque);
    for (int i = 0; i < 200; ++i)
        assert(history->append(concurrent, SensorHistory::Humidity, 4000 + i, 1700500000L + i * 60L));
    return 0;
}
unsigned char testCrc8(const unsigned char* bytes, unsigned long length) {
    unsigned char crc = 0;
    for (unsigned long i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? static_cast<unsigned char>((crc << 1) ^ 0x07) : static_cast<unsigned char>(crc << 1);
    }
    return crc;
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

    SensorHistory concurrentHistory(path);
    assert(concurrentHistory.initialize(&error));
    pthread_t writer;
    assert(pthread_create(&writer, 0, writeConcurrent, &concurrentHistory) == 0);
    for (int i = 0; i < 50; ++i) {
        assert(concurrentHistory.readForDevice(concurrent, 0, &json, &error));
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
    assert(json.find("\"samples\":[") == 1 && json.find("\"samples\":[]") == std::string::npos);
    bool foundOldValue = false;
    unsigned long cursor = nextBefore(json);
    while (cursor != 0) {
        assert(SensorHistory::readJson(path, sensor, cursor, &json, &error));
        if (json.find("-1900") != std::string::npos) foundOldValue = true;
        cursor = nextBefore(json);
    }
    assert(foundOldValue);
    removeHistoryFiles(path);

    path = historyPath();
    {
        SensorHistory history(path);
        assert(history.append(sensor, SensorHistory::Humidity, 4300, 1700000000));
    }
    /* Page ownership is bound into every record checksum. Re-sign changed
       metadata, leaving the record checksum stale, and the sample is rejected. */
    int fd = open(path.c_str(), O_RDWR);
    assert(fd >= 0);
    unsigned char changedAddressByte = 0x99;
    assert(pwrite(fd, &changedAddressByte, 1, 16) == 1);
    unsigned char pageHeader[16];
    assert(pread(fd, pageHeader, sizeof(pageHeader), 16) == sizeof(pageHeader));
    pageHeader[14] = testCrc8(pageHeader, 14);
    assert(pwrite(fd, pageHeader, sizeof(pageHeader), 16) == sizeof(pageHeader));
    close(fd);
    assert(SensorHistory::readJson(path, sensor, 0, &json, &error));
    assert(json == "{\"samples\":[],\"next_before\":0}");
    removeHistoryFiles(path);

    path = historyPath();
    {
        SensorHistory history(path);
        assert(history.append(sensor, SensorHistory::Humidity, 5100, 1700000000));
    }
    fd = open(path.c_str(), O_RDWR);
    assert(fd >= 0);
    unsigned char generationHeader[16];
    assert(pread(fd, generationHeader, sizeof(generationHeader), 16) == sizeof(generationHeader));
    generationHeader[13] ^= 1;
    generationHeader[14] = testCrc8(generationHeader, 14);
    assert(pwrite(fd, generationHeader, sizeof(generationHeader), 16) == sizeof(generationHeader));
    close(fd);
    assert(SensorHistory::readJson(path, sensor, 0, &json, &error));
    assert(json == "{\"samples\":[],\"next_before\":0}");
    removeHistoryFiles(path);

    path = historyPath();
    {
        SensorHistory history(path);
        assert(history.append(sensor, SensorHistory::Humidity, 5000, 1703500000L));
    }
    fd = open(path.c_str(), O_RDWR);
    assert(fd >= 0);
    unsigned char metricHeader[16];
    assert(pread(fd, metricHeader, sizeof(metricHeader), 16) == sizeof(metricHeader));
    metricHeader[8] = SensorHistory::Temperature;
    metricHeader[14] = testCrc8(metricHeader, 14);
    assert(pwrite(fd, metricHeader, sizeof(metricHeader), 16) == sizeof(metricHeader));
    close(fd);
    assert(SensorHistory::readJson(path, sensor, 0, &json, &error));
    assert(json == "{\"samples\":[],\"next_before\":0}");
    removeHistoryFiles(path);

    path = historyPath();
    int unsupported = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(unsupported >= 0 && write(unsupported, "OLD!unsupported", 15) == 15);
    close(unsupported);
    {
        SensorHistory history(path);
        assert(!history.initialize(&error));
        assert(error.find("file preserved") != std::string::npos);
    }
    assert(stat(path.c_str(), &info) == 0 && info.st_size == 15);
    removeHistoryFiles(path);

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
    removeHistoryFiles(path);

    path = historyPath();
    {
        SensorHistory history(path);
        assert(history.append(sensor, SensorHistory::Humidity, 5000, 1704000000L));
        assert(history.append(other, SensorHistory::Power, 120, 1704000000L));
        SensorHistory::ReadStats stats;
        struct timeval start, finish;
        gettimeofday(&start, 0);
        assert(history.readForDevice(sensor, 0, &json, &error, &stats));
        gettimeofday(&finish, 0);
        assert(stats.pagesRead == 1 && stats.recordsChecked == 128 && stats.bytesRead == 1424);
        fprintf(stderr, "sensor history targeted read: pages=%lu bytes=%lu elapsed_us=%lu\n",
                stats.pagesRead, stats.bytesRead,
                static_cast<unsigned long>((finish.tv_sec - start.tv_sec) * 1000000L + finish.tv_usec - start.tv_usec));
    }
    removeHistoryFiles(path);

    path = historyPath();
    {
        SensorHistory history(path);
        std::vector<std::string> active;
        char addressBytes[32];
        for (unsigned long i = 0; i < 1472; ++i) {
            snprintf(addressBytes, sizeof(addressBytes), "0x%016lx", i + 1);
            active.push_back(addressBytes);
            assert(history.append(addressBytes, SensorHistory::Temperature, static_cast<int>(i % 2000),
                                  1705000000L + static_cast<long>(i) * 60L));
        }
        history.setActiveDevices(active);
        struct stat fullPool;
        assert(stat(path.c_str(), &fullPool) == 0 && fullPool.st_size == 16 + 1472 * 1424);
        SensorHistory::ReadStats fullPoolStats;
        struct timeval fullStart, fullFinish;
        gettimeofday(&fullStart, 0);
        assert(history.readForDevice("0x0000000000000001", 0, &json, &error, &fullPoolStats));
        gettimeofday(&fullFinish, 0);
        assert(fullPoolStats.pagesRead == 1 && fullPoolStats.recordsChecked == 128 &&
               fullPoolStats.bytesRead == 1424);
        fprintf(stderr, "sensor history 1,472-page lookup: pages=%lu bytes=%lu elapsed_us=%lu\n",
                fullPoolStats.pagesRead, fullPoolStats.bytesRead,
                static_cast<unsigned long>((fullFinish.tv_sec - fullStart.tv_sec) * 1000000L +
                                           fullFinish.tv_usec - fullStart.tv_usec));
        /* All 1,472 baseline pages are the only page for an active pair. A
           new pair gets an overflow page instead of losing another device. */
        assert(history.append("0x0000000000005c00", SensorHistory::Humidity, 4400, 1706000000L));
        assert(stat(path.c_str(), &fullPool) == 0 && fullPool.st_size == 16 + 1473 * 1424);
        assert(history.readForDevice("0x0000000000000001", 0, &json, &error));
        assert(json.find(",1,0]") != std::string::npos);
        assert(history.readForDevice("0x0000000000005c00", 0, &json, &error));
        assert(json.find(",2,4400]") != std::string::npos);
        /* Reuse the new pair's single page after 128 samples; it retains its
           newest 128 records without borrowing an active pair's only page. */
        for (int i = 1; i < 129; ++i)
            assert(history.append("0x0000000000005c00", SensorHistory::Humidity, 4400 + i,
                                  1706000000L + static_cast<long>(i) * 60L));
        assert(history.readForDevice("0x0000000000005c00", 0, &json, &error));
        assert(json.find(",2,4400]") == std::string::npos);
        assert(json.find(",2,4528]") != std::string::npos);
        /* Removal makes the old page eligible for eviction. */
        history.setDevicePresent("0x0000000000000001", false);
        assert(history.append("0x0000000000005c01", SensorHistory::Temperature, 2, 1707000000L));
        assert(history.readForDevice("0x0000000000000001", 0, &json, &error));
        assert(json == "{\"samples\":[],\"next_before\":0}");
        assert(history.readForDevice("0x0000000000000002", 0, &json, &error));
        assert(json.find("\"samples\":[") == 1);
    }
    removeHistoryFiles(path);

    const std::string matterId = "matter:0000000000000001:0000000000000002:0001";
    const std::string secondMatterId = "matter:0000000000000001:0000000000000002:0002";
    const std::string shellyId = "shelly:0011223344556677:temperature:2";
    {
        SensorHistory history(path);
        assert(history.appendValue(matterId, SensorHistory::Temperature, "-5.25", "celsius", 1708000000L));
        assert(history.appendValue(matterId, SensorHistory::Humidity, "45.5", "percent", 1708000000L));
        assert(history.appendValue(matterId, SensorHistory::CarbonDioxide, "758.5", "ppb", 1708000000L));
        assert(history.appendValue(matterId, SensorHistory::Pm25, "12.75", "ug/m3", 1708000000L));
        assert(history.appendValue(matterId, SensorHistory::AirQuality, "moderate", "", 1708000001L));
        assert(history.appendValue(matterId, SensorHistory::Light, "1", "", 1708000002L));
        assert(history.appendValue(matterId, SensorHistory::Brightness, "50", "percent", 1708000003L));
        assert(history.appendValue(secondMatterId, SensorHistory::Temperature, "21.5", "celsius", 1708000000L));
        assert(history.appendValue(shellyId, SensorHistory::Temperature, "22.375", "celsius", 1708000000L));
        assert(history.appendValue(shellyId, SensorHistory::Contact, "1", "", 1708000001L));
        assert(history.appendValue(shellyId, SensorHistory::Power, "42.3", "watts", 1708000002L));
        assert(!history.appendValue(shellyId, SensorHistory::Power, "42.3", "kW", 1708000060L));
        assert(!history.appendValue(matterId, SensorHistory::CarbonDioxide, "758.5", "miles", 1708000060L));
        assert(history.readForDevice(matterId, 0, &json, &error));
        assert(json.find("-5.25,\"celsius\"") != std::string::npos);
        assert(json.find("758.5,\"ppb\"") != std::string::npos);
        assert(json.find("\"moderate\",\"\"") != std::string::npos);
        assert(json.find("21.5") == std::string::npos);
        assert(history.readForDevice(shellyId, 0, &json, &error));
        assert(json.find("22.375,\"celsius\"") != std::string::npos);
        assert(json.find(",3,1,\"\"") != std::string::npos);
        assert(json.find("42.3,\"watts\"") != std::string::npos);
    }
    {
        SensorHistory restarted(path);
        assert(restarted.initialize(&error));
        assert(restarted.readForDevice(matterId, 0, &json, &error));
        assert(json.find("12.75,\"ug/m3\"") != std::string::npos);
    }

    std::string boundedPath = historyPath();
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
