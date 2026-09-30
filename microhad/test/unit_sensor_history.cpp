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
void put32be(unsigned char* bytes, uint32_t value) {
    bytes[0] = static_cast<unsigned char>(value >> 24); bytes[1] = static_cast<unsigned char>(value >> 16);
    bytes[2] = static_cast<unsigned char>(value >> 8); bytes[3] = static_cast<unsigned char>(value);
}
void writeLegacyFixture(const std::string& path) {
    unsigned char header[16] = {'T','Z','H','1',1,20,0,0,0,0,0,0,0,0,0,0};
    unsigned char record[20];
    memset(record, 0, sizeof(record));
    put32be(record, 1); put32be(record + 4, 1703000000U);
    const unsigned char address[8] = {0,0x12,0x4b,0,0,0,0,0x42};
    memcpy(record + 8, address, 8); record[16] = SensorHistory::Humidity;
    record[17] = 0x13; record[18] = 0x88; record[19] = testCrc8(record, 19);
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, header, sizeof(header)) == sizeof(header));
    assert(write(fd, record, sizeof(record)) == sizeof(record));
    close(fd);
}
void putLegacyRecord(unsigned char* record, uint32_t sequence, const char* address,
                     unsigned char metric, unsigned int value) {
    memset(record, 0, 20);
    put32be(record, sequence); put32be(record + 4, 1703000000U + sequence);
    for (int i = 0; i < 8; ++i) {
        char octet[3] = {address[2 + i * 2], address[3 + i * 2], 0};
        record[8 + i] = static_cast<unsigned char>(strtoul(octet, 0, 16));
    }
    record[16] = metric; record[17] = static_cast<unsigned char>(value >> 8);
    record[18] = static_cast<unsigned char>(value); record[19] = testCrc8(record, 19);
}
void writeWrappedLegacyFixture(const std::string& path) {
    const unsigned long capacity = (2 * 1024 * 1024 - 16) / 20;
    unsigned char header[16] = {'T','Z','H','1',1,20,0,0,0,0,0,0,0,0,0,0};
    unsigned char record[20];
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, header, sizeof(header)) == sizeof(header));
    assert(ftruncate(fd, 16 + capacity * 20) == 0);
    putLegacyRecord(record, static_cast<uint32_t>(capacity), sensor, SensorHistory::Humidity, 4300);
    assert(pwrite(fd, record, sizeof(record), 16 + (capacity - 1) * 20) == sizeof(record));
    putLegacyRecord(record, static_cast<uint32_t>(capacity + 1), sensor, SensorHistory::Humidity, 4400);
    assert(pwrite(fd, record, sizeof(record), 16) == sizeof(record));
    putLegacyRecord(record, 2, other, SensorHistory::Temperature, 2100);
    record[19] ^= 1; /* damaged record must not be migrated */
    assert(pwrite(fd, record, sizeof(record), 16 + 20) == sizeof(record));
    close(fd);
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
    unlink(path.c_str());

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
    unlink(path.c_str());

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
    unlink(path.c_str());

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
    unlink(path.c_str());

    path = historyPath();
    writeLegacyFixture(path);
    assert(SensorHistory::readJson(path, sensor, 0, &json, &error));
    assert(json.find(",2,5000]") != std::string::npos);
    std::string backup = path + ".v1.bak";
    assert(access(backup.c_str(), F_OK) == 0);
    assert(stat(path.c_str(), &info) == 0 && info.st_size == 16 + 1424);
    unlink(path.c_str()); unlink(backup.c_str());

    path = historyPath();
    writeWrappedLegacyFixture(path);
    assert(SensorHistory::readJson(path, sensor, 0, &json, &error));
    assert(json.find(",2,4300]") != std::string::npos && json.find(",2,4400]") != std::string::npos);
    assert(SensorHistory::readJson(path, other, 0, &json, &error));
    assert(json == "{\"samples\":[],\"next_before\":0}");
    backup = path + ".v1.bak";
    unlink(path.c_str()); unlink(backup.c_str());

    /* Simulate the first rename in a conversion transaction: V2 temp and the
       untouched V1 backup exist while the active path is absent. */
    path = historyPath();
    {
        SensorHistory history(path);
        assert(history.append(sensor, SensorHistory::Humidity, 5500, 1703100000L));
    }
    std::string temporary = path + ".v2.tmp";
    std::string backupPath = path + ".v1.bak";
    assert(rename(path.c_str(), temporary.c_str()) == 0);
    writeLegacyFixture(backupPath);
    {
        SensorHistory recovered(path);
        assert(recovered.initialize(&error));
        assert(recovered.readForDevice(sensor, 0, &json, &error));
        assert(json.find(",2,5500]") != std::string::npos);
    }
    assert(access(backupPath.c_str(), F_OK) == 0);
    unlink(path.c_str()); unlink(temporary.c_str()); unlink(backupPath.c_str());

    /* If the active file vanished with a damaged temporary but an untouched
       legacy backup remains, restore the backup and retry conversion. */
    path = historyPath();
    temporary = path + ".v2.tmp";
    backupPath = path + ".v1.bak";
    writeLegacyFixture(backupPath);
    int damaged = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(damaged >= 0 && write(damaged, "partial", 7) == 7);
    close(damaged);
    {
        SensorHistory recovered(path);
        assert(recovered.initialize(&error));
        assert(recovered.readForDevice(sensor, 0, &json, &error));
        assert(json.find(",2,5000]") != std::string::npos);
    }
    backupPath = path + ".v1.bak";
    unlink(path.c_str()); unlink(temporary.c_str()); unlink(backupPath.c_str());

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
    unlink(path.c_str());

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
    unlink(path.c_str());
}
