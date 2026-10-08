#include "sensor_history.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
enum { valueHeaderBytes = 16, valueRecordBytes = 128, valueRecordLimit = 8192,
       valueKeepAfterCompaction = 4096, valuePageLimit = 2000 };
const unsigned char valueFileTemplate[valueHeaderBytes] = {
    'T', 'U', 'V', '1', 1, valueHeaderBytes, valueRecordBytes, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};
void put32(unsigned char* bytes, uint32_t value) {
    bytes[0] = static_cast<unsigned char>(value >> 24);
    bytes[1] = static_cast<unsigned char>(value >> 16);
    bytes[2] = static_cast<unsigned char>(value >> 8);
    bytes[3] = static_cast<unsigned char>(value);
}
uint32_t get32(const unsigned char* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}
unsigned char crc8(const unsigned char* bytes, unsigned long length) {
    unsigned char crc = 0;
    for (unsigned long i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? static_cast<unsigned char>((crc << 1) ^ 0x07) : static_cast<unsigned char>(crc << 1);
    }
    return crc;
}
bool writeAllAt(int fd, const unsigned char* bytes, unsigned long length, off_t offset) {
    unsigned long written = 0;
    while (written < length) {
        ssize_t count = pwrite(fd, bytes + written, length - written, offset + written);
        if (count <= 0) return false;
        written += static_cast<unsigned long>(count);
    }
    return true;
}
std::string valuePairKey(const std::string& deviceId, unsigned char metric) {
    std::string key = deviceId;
    key += '\0';
    key += static_cast<char>(metric);
    return key;
}
bool valueFileHeaderValid(const unsigned char* bytes) {
    unsigned char expected[valueHeaderBytes];
    memcpy(expected, valueFileTemplate, sizeof(expected));
    expected[valueHeaderBytes - 1] = crc8(expected, valueHeaderBytes - 1);
    return memcmp(bytes, expected, sizeof(expected)) == 0;
}
bool plainText(const std::string& value, unsigned long maximum, bool allowEmpty) {
    if ((!allowEmpty && value.empty()) || value.size() > maximum) return false;
    for (unsigned long i = 0; i < value.size(); ++i)
        if (static_cast<unsigned char>(value[i]) < 0x20 || static_cast<unsigned char>(value[i]) > 0x7e) return false;
    return true;
}
bool valueMetricValid(unsigned char metric, const std::string& value, const std::string& unit) {
    if (!plainText(value, 24, false) || !plainText(unit, 16, true)) return false;
    if (metric == SensorHistory::Contact || metric == SensorHistory::Occupancy)
        return unit.empty() && (value == "0" || value == "1");
    if (metric == SensorHistory::Battery) {
        char* batteryEnd = 0;
        const double battery = std::strtod(value.c_str(), &batteryEnd);
        return unit == "percent" && batteryEnd == value.c_str() + value.size() &&
               std::isfinite(battery) && battery >= 0 && battery <= 100;
    }
    if (metric == SensorHistory::AirQuality)
        return unit.empty() && (value == "good" || value == "fair" || value == "moderate" || value == "poor" ||
            value == "very_poor" || value == "extremely_poor");
    if (metric == SensorHistory::Light) return unit.empty() && (value == "0" || value == "1");
    if (metric < SensorHistory::Temperature || metric > SensorHistory::Brightness || metric == SensorHistory::Battery) return false;
    char* end = 0;
    const double numeric = std::strtod(value.c_str(), &end);
    if (end != value.c_str() + value.size() || !std::isfinite(numeric)) return false;
    if (metric == SensorHistory::Temperature) return unit == "celsius" && numeric >= -327.68 && numeric <= 327.67;
    if (metric == SensorHistory::Humidity) return unit == "percent" && numeric >= 0 && numeric <= 100;
    if (metric == SensorHistory::Power) return unit == "watts" && numeric >= 0 && numeric <= 6553.5;
    if (metric == SensorHistory::CarbonDioxide) {
        static const char* const units[] = {"ppm", "ppb", "ppt", "mg/m3", "ug/m3", "ng/m3",
            "particles/m3", "Bq/m3", "dB/m", "particles/ft3"};
        for (unsigned int i = 0; i < sizeof(units) / sizeof(units[0]); ++i)
            if (unit == units[i]) return numeric >= 0;
        return false;
    }
    if (metric == SensorHistory::Pm25) {
        static const char* const units[] = {"ppm", "ppb", "ppt", "mg/m3", "ug/m3", "ng/m3",
            "particles/m3", "Bq/m3", "dB/m", "particles/ft3"};
        for (unsigned int i = 0; i < sizeof(units) / sizeof(units[0]); ++i)
            if (unit == units[i]) return numeric >= 0;
        return false;
    }
    if (metric == SensorHistory::Brightness) return unit == "percent" && numeric >= 0 && numeric <= 100;
    return false;
}
bool parseValueRecord(const unsigned char* record, std::string* deviceId, unsigned char* metric,
                      std::string* value, std::string* unit, unsigned long* sequence, long* timestamp) {
    if (record[127] != crc8(record, 127) || record[4] < SensorHistory::Temperature ||
        record[4] > SensorHistory::Brightness || record[5] == 0 || record[5] > 64 ||
        record[6] > 16 || record[7] == 0 || record[7] > 24) return false;
    *sequence = get32(record + 8);
    *timestamp = static_cast<long>(get32(record + 12));
    if (*sequence == 0 || *timestamp <= 0) return false;
    deviceId->assign(reinterpret_cast<const char*>(record + 16), record[5]);
    unit->assign(reinterpret_cast<const char*>(record + 80), record[6]);
    value->assign(reinterpret_cast<const char*>(record + 96), record[7]);
    *metric = record[4];
    return plainText(*deviceId, 64, false) && valueMetricValid(*metric, *value, *unit);
}
void encodeValueRecord(unsigned char* record, const std::string& deviceId, unsigned char metric,
                       const std::string& value, const std::string& unit,
                       unsigned long sequence, long timestamp) {
    memset(record, 0, valueRecordBytes);
    record[4] = metric;
    record[5] = static_cast<unsigned char>(deviceId.size());
    record[6] = static_cast<unsigned char>(unit.size());
    record[7] = static_cast<unsigned char>(value.size());
    put32(record + 8, static_cast<uint32_t>(sequence));
    put32(record + 12, static_cast<uint32_t>(timestamp));
    memcpy(record + 16, deviceId.data(), deviceId.size());
    memcpy(record + 80, unit.data(), unit.size());
    memcpy(record + 96, value.data(), value.size());
    record[127] = crc8(record, 127);
}
std::string jsonString(const std::string& value) {
    std::string output = "\"";
    for (unsigned long i = 0; i < value.size(); ++i) {
        if (value[i] == '"' || value[i] == '\\') output += '\\';
        output += value[i];
    }
    output += '"';
    return output;
}
struct ValueCompactRecord {
    std::string key;
    unsigned long sequence;
    off_t offset;
};
bool olderValueRecord(const ValueCompactRecord& left, const ValueCompactRecord& right) {
    return left.sequence < right.sequence;
}
}

SensorHistory::ValueJournal::ValueJournal() : sequence(0), recordCount(0), initialized(false) {
    pthread_mutex_init(&mutex, 0);
}
SensorHistory::ValueJournal::~ValueJournal() { pthread_mutex_destroy(&mutex); }
SensorHistory::SensorHistory(const std::string& path) : obsoleteHistoryPath_(path) { values_.path = path + ".values"; }
SensorHistory::~SensorHistory() {}
const std::string& SensorHistory::path() const { return values_.path; }

bool SensorHistory::initialize(std::string* error) {
    if (error != 0) error->clear();
    /* Drop the obsolete paged file; the source-neutral journal is the only store. */
    return initializeValues(error);
}

bool SensorHistory::initializeValues(std::string* error) {
    pthread_mutex_lock(&values_.mutex);
    if (values_.initialized) { pthread_mutex_unlock(&values_.mutex); return true; }
    unlink(obsoleteHistoryPath_.c_str());
    int fd = open(values_.path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        if (error) *error = "cannot open source-neutral history";
        pthread_mutex_unlock(&values_.mutex);
        return false;
    }
    struct stat info;
    bool ok = fstat(fd, &info) == 0;
    if (ok && info.st_size == 0) {
        unsigned char header[valueHeaderBytes];
        memcpy(header, valueFileTemplate, sizeof(header));
        header[valueHeaderBytes - 1] = crc8(header, valueHeaderBytes - 1);
        ok = writeAllAt(fd, header, sizeof(header), 0);
        info.st_size = valueHeaderBytes;
    }
    unsigned char header[valueHeaderBytes];
    if (ok) ok = pread(fd, header, sizeof(header), 0) == valueHeaderBytes && valueFileHeaderValid(header);
    if (!ok) {
        if (error) *error = "invalid source-neutral history header; file preserved";
        close(fd);
        pthread_mutex_unlock(&values_.mutex);
        return false;
    }
    values_.records.clear();
    values_.lastValues.clear();
    values_.lastMinutes.clear();
    values_.sequence = 0;
    values_.recordCount = 0;
    off_t completeEnd = valueHeaderBytes + ((info.st_size - valueHeaderBytes) / valueRecordBytes) * valueRecordBytes;
    for (off_t offset = valueHeaderBytes; offset < completeEnd; offset += valueRecordBytes) {
        unsigned char record[valueRecordBytes];
        if (pread(fd, record, sizeof(record), offset) != static_cast<ssize_t>(sizeof(record))) continue;
        std::string deviceId, value, unit;
        unsigned char metric = 0;
        unsigned long sequence = 0;
        long timestamp = 0;
        if (!parseValueRecord(record, &deviceId, &metric, &value, &unit, &sequence, &timestamp)) continue;
        const std::string key = valuePairKey(deviceId, metric);
        values_.records[key].push_back(ValueRecord(sequence, offset));
        values_.lastValues[key] = value + '\0' + unit;
        if (metric != Contact && metric != Occupancy && metric != AirQuality && metric != Light)
            values_.lastMinutes[key] = timestamp / 60;
        if (sequence > values_.sequence) values_.sequence = sequence;
        ++values_.recordCount;
    }
    if (completeEnd != info.st_size && ftruncate(fd, completeEnd) != 0) {
        if (error) *error = "cannot repair incomplete source-neutral history tail";
        close(fd);
        pthread_mutex_unlock(&values_.mutex);
        return false;
    }
    close(fd);
    values_.initialized = true;
    pthread_mutex_unlock(&values_.mutex);
    return true;
}

bool SensorHistory::appendValue(const std::string& deviceId, unsigned char metric, const std::string& value,
                                const std::string& unit, long timestamp) {
    if (!values_.initialized && !initializeValues(0)) return false;
    pthread_mutex_lock(&values_.mutex);
    const bool result = appendValueLocked(deviceId, metric, value, unit, timestamp);
    pthread_mutex_unlock(&values_.mutex);
    return result;
}

bool SensorHistory::appendValueLocked(const std::string& deviceId, unsigned char metric, const std::string& value,
                                     const std::string& unit, long timestamp) {
    if (!values_.initialized || !plainText(deviceId, 64, false) ||
        !valueMetricValid(metric, value, unit) || timestamp <= 0 ||
        static_cast<unsigned long>(timestamp) > 0xffffffffUL) return false;
    const std::string key = valuePairKey(deviceId, metric);
    std::map<std::string, std::string>::const_iterator previous = values_.lastValues.find(key);
    if (previous != values_.lastValues.end() && previous->second == value + '\0' + unit) return true;
    if (metric != Contact && metric != Occupancy && metric != AirQuality && metric != Light) {
        std::map<std::string, long>::const_iterator minute = values_.lastMinutes.find(key);
        if (minute != values_.lastMinutes.end() && timestamp / 60 <= minute->second) return true;
    }

    if (values_.recordCount >= valueRecordLimit) {
        int source = open(values_.path.c_str(), O_RDONLY);
        std::vector<ValueCompactRecord> candidates;
        for (std::map<std::string, std::vector<ValueRecord> >::const_iterator pair = values_.records.begin();
             pair != values_.records.end(); ++pair)
            for (unsigned long i = 0; i < pair->second.size(); ++i) {
                ValueCompactRecord current = {pair->first, pair->second[i].sequence, pair->second[i].offset};
                candidates.push_back(current);
            }
        std::sort(candidates.begin(), candidates.end(), olderValueRecord);
        std::map<std::string, unsigned int> retainedPerPair;
        std::vector<ValueCompactRecord> retained;
        for (unsigned long i = candidates.size(); i > 0 && retained.size() < valueKeepAfterCompaction; --i) {
            ValueCompactRecord& candidate = candidates[i - 1];
            if (retainedPerPair[candidate.key] >= 128) continue;
            ++retainedPerPair[candidate.key];
            retained.push_back(candidate);
        }
        std::sort(retained.begin(), retained.end(), olderValueRecord);
        std::string temporaryPath = values_.path + ".tmp";
        int destination = open(temporaryPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        unsigned char header[valueHeaderBytes];
        memcpy(header, valueFileTemplate, sizeof(header));
        header[valueHeaderBytes - 1] = crc8(header, valueHeaderBytes - 1);
        bool compacted = source >= 0 && destination >= 0 && writeAllAt(destination, header, sizeof(header), 0);
        unsigned char record[valueRecordBytes];
        if (compacted) {
            for (unsigned long i = 0; i < retained.size(); ++i) {
                if (pread(source, record, sizeof(record), retained[i].offset) != static_cast<ssize_t>(sizeof(record)) ||
                    !writeAllAt(destination, record, sizeof(record), valueHeaderBytes +
                                static_cast<off_t>(i * valueRecordBytes))) { compacted = false; break; }
            }
        }
        if (source >= 0) close(source);
        if (destination >= 0) close(destination);
        if (!compacted || rename(temporaryPath.c_str(), values_.path.c_str()) != 0) {
            unlink(temporaryPath.c_str());
            return false;
        }
        int compactedFile = open(values_.path.c_str(), O_RDONLY);
        if (compactedFile < 0) { values_.initialized = false; return false; }
        std::map<std::string, std::vector<ValueRecord> > compactedRecords;
        std::map<std::string, std::string> compactedLastValues;
        std::map<std::string, long> compactedLastMinutes;
        for (unsigned long i = 0; i < retained.size(); ++i) {
            unsigned char saved[valueRecordBytes];
            if (pread(compactedFile, saved, sizeof(saved), valueHeaderBytes +
                      static_cast<off_t>(i * valueRecordBytes)) != static_cast<ssize_t>(sizeof(saved))) continue;
            std::string savedId, savedValue, savedUnit;
            unsigned char savedMetric = 0;
            unsigned long savedSequence = 0;
            long savedTimestamp = 0;
            if (!parseValueRecord(saved, &savedId, &savedMetric, &savedValue, &savedUnit,
                                  &savedSequence, &savedTimestamp)) continue;
            const std::string savedKey = valuePairKey(savedId, savedMetric);
            compactedRecords[savedKey].push_back(ValueRecord(savedSequence, valueHeaderBytes +
                static_cast<off_t>(i * valueRecordBytes)));
            compactedLastValues[savedKey] = savedValue + '\0' + savedUnit;
            if (savedMetric != Contact && savedMetric != Occupancy && savedMetric != AirQuality && savedMetric != Light)
                compactedLastMinutes[savedKey] = savedTimestamp / 60;
        }
        close(compactedFile);
        values_.records.swap(compactedRecords);
        values_.lastValues.swap(compactedLastValues);
        values_.lastMinutes.swap(compactedLastMinutes);
        values_.recordCount = retained.size();
    }
    if (values_.sequence >= 0xffffffffUL) return false;
    const unsigned long sequence = values_.sequence + 1;
    unsigned char record[valueRecordBytes];
    encodeValueRecord(record, deviceId, metric, value, unit, sequence, timestamp);
    int fd = open(values_.path.c_str(), O_RDWR);
    struct stat info;
    bool ok = fd >= 0 && fstat(fd, &info) == 0 &&
        info.st_size >= valueHeaderBytes && (info.st_size - valueHeaderBytes) % valueRecordBytes == 0;
    off_t offset = ok ? info.st_size : 0;
    if (ok) ok = writeAllAt(fd, record, sizeof(record), offset);
    if (fd >= 0) close(fd);
    if (!ok) return false;
    values_.sequence = sequence;
    ++values_.recordCount;
    values_.records[key].push_back(ValueRecord(sequence, offset));
    values_.lastValues[key] = value + '\0' + unit;
    if (metric != Contact && metric != Occupancy && metric != AirQuality && metric != Light)
        values_.lastMinutes[key] = timestamp / 60;
    return true;
}

bool SensorHistory::readValuesJson(const std::string& deviceId, unsigned long before,
                                   std::string* json, std::string* error, ReadStats* stats) const {
    if (!plainText(deviceId, 64, false)) { *error = "invalid device ID"; return false; }
    if (!values_.initialized && !const_cast<SensorHistory*>(this)->initializeValues(error)) return false;
    std::vector<ValueCompactRecord> selected;
    pthread_mutex_lock(&values_.mutex);
    if (!values_.initialized) {
        pthread_mutex_unlock(&values_.mutex);
        *error = "source-neutral history catalog is unavailable";
        return false;
    }
    for (unsigned char metric = Temperature; metric <= Brightness; ++metric) {
        const std::string key = valuePairKey(deviceId, metric);
        std::map<std::string, std::vector<ValueRecord> >::const_iterator found = values_.records.find(key);
        if (found == values_.records.end()) continue;
        for (unsigned long i = 0; i < found->second.size(); ++i) {
            if (before != 0 && found->second[i].sequence >= before) continue;
            ValueCompactRecord sample = {key, found->second[i].sequence, found->second[i].offset};
            selected.push_back(sample);
        }
    }
    std::sort(selected.begin(), selected.end(), olderValueRecord);
    bool more = selected.size() > valuePageLimit;
    if (more) selected.erase(selected.begin(), selected.end() - valuePageLimit);
    int fd = open(values_.path.c_str(), O_RDONLY);
    if (fd < 0) {
        pthread_mutex_unlock(&values_.mutex);
        *error = "cannot read source-neutral history";
        return false;
    }
    unsigned char header[valueHeaderBytes];
    struct stat info;
    if (fstat(fd, &info) != 0 || pread(fd, header, sizeof(header), 0) != valueHeaderBytes ||
        !valueFileHeaderValid(header)) {
        close(fd);
        pthread_mutex_unlock(&values_.mutex);
        *error = "invalid source-neutral history file";
        return false;
    }
    std::ostringstream output;
    output << "{\"samples\":[";
    bool first = true;
    for (unsigned long i = 0; i < selected.size(); ++i) {
        unsigned char record[valueRecordBytes];
        if (pread(fd, record, sizeof(record), selected[i].offset) != static_cast<ssize_t>(sizeof(record))) continue;
        std::string savedId, value, unit;
        unsigned char metric = 0;
        unsigned long sequence = 0;
        long timestamp = 0;
        if (!parseValueRecord(record, &savedId, &metric, &value, &unit, &sequence, &timestamp) ||
            savedId != deviceId || sequence != selected[i].sequence) continue;
        if (stats) { ++stats->recordsChecked; stats->bytesRead += valueRecordBytes; }
        if (stats) ++stats->pagesRead;
        if (!first) output << ',';
        first = false;
        output << '[' << sequence << ',' << timestamp << ',' << static_cast<unsigned int>(metric) << ',';
        if (metric == AirQuality) output << jsonString(value); else output << value;
        output << ',' << jsonString(unit) << ']';
    }
    output << "],\"next_before\":" << (more && !selected.empty() ? selected.front().sequence : 0) << '}';
    *json = output.str();
    close(fd);
    pthread_mutex_unlock(&values_.mutex);
    return true;
}

bool SensorHistory::readForDevice(const std::string& address, unsigned long before,
        std::string* json, std::string* error, ReadStats* stats) const {
    const std::string::size_type separator = address.find(':');
    if (separator == std::string::npos) {
        if (address.size() != 18 || address[0] != '0' || address[1] != 'x') {
            if (error) *error = "invalid device address";
            return false;
        }
        for (unsigned long i = 2; i < address.size(); ++i) {
            const char value = address[i];
            if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F'))) {
                if (error) *error = "invalid device address";
                return false;
            }
        }
    }
    const std::string deviceId = separator == std::string::npos ? "zigbee:" + address : address;
    return readValuesJson(deviceId, before, json, error, stats);
}

bool SensorHistory::readJson(const std::string& path, const std::string& address,
        unsigned long before, std::string* json, std::string* error, ReadStats* stats) {
    SensorHistory reader(path);
    if (!reader.initialize(error)) return false;
    return reader.readForDevice(address, before, json, error, stats);
}
