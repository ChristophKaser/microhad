#include "sensor_history.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
enum { headerBytes = 16, recordBytes = 20, maximumBytes = 512 * 1024,
       capacity = (maximumBytes - headerBytes) / recordBytes, pageLimit = 2000 };
const unsigned char header[headerBytes] = { 'T', 'Z', 'H', '1', 1, recordBytes, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

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
unsigned char checksum(const unsigned char* bytes) {
    unsigned char crc = 0;
    for (int i = 0; i < recordBytes - 1; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80) ? static_cast<unsigned char>((crc << 1) ^ 0x07) : static_cast<unsigned char>(crc << 1);
    }
    return crc;
}
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool decodeAddress(const std::string& address, unsigned char* bytes) {
    if (address.size() != 18 || address[0] != '0' || address[1] != 'x') return false;
    for (int i = 0; i < 8; ++i) {
        int high = hexDigit(address[2 + i * 2]);
        int low = hexDigit(address[3 + i * 2]);
        if (high < 0 || low < 0) return false;
        bytes[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}
std::string addressKey(const unsigned char* bytes, unsigned char metric) {
    const char digits[] = "0123456789abcdef";
    std::string result = "0x";
    for (int i = 0; i < 8; ++i) {
        result += digits[bytes[i] >> 4];
        result += digits[bytes[i] & 15];
    }
    result += static_cast<char>(metric);
    return result;
}
bool validHeader(int fd, off_t size) {
    unsigned char bytes[headerBytes];
    return size >= headerBytes && size <= maximumBytes && pread(fd, bytes, sizeof(bytes), 0) == headerBytes &&
           memcmp(bytes, header, headerBytes) == 0;
}
struct Sample {
    uint32_t sequence;
    uint32_t timestamp;
    unsigned char metric;
    int value;
};
bool validRecord(const unsigned char* bytes, unsigned long slot, Sample* sample) {
    uint32_t sequence = get32(bytes);
    if (sequence == 0 || (sequence - 1) % capacity != slot || bytes[19] != checksum(bytes) ||
        bytes[16] < SensorHistory::Temperature || bytes[16] > SensorHistory::Battery) return false;
    sample->sequence = sequence;
    sample->timestamp = get32(bytes + 4);
    sample->metric = bytes[16];
    unsigned int raw = (static_cast<unsigned int>(bytes[17]) << 8) | bytes[18];
    sample->value = (sample->metric == SensorHistory::Power || sample->metric == SensorHistory::Battery) ? static_cast<int>(raw) : static_cast<int16_t>(raw);
    if ((sample->metric == SensorHistory::Contact || sample->metric == SensorHistory::Occupancy) &&
        (sample->value < 0 || sample->value > 1)) return false;
    return true;
}
bool newer(const Sample& left, const Sample& right) { return left.sequence > right.sequence; }
void rememberMinute(std::map<std::string, long>* minutes, const std::string& key, long minute) {
    std::map<std::string, long>::iterator existing = minutes->find(key);
    if (existing != minutes->end()) {
        if (minute > existing->second) existing->second = minute;
        return;
    }
    if (minutes->size() == 128) {
        std::map<std::string, long>::iterator oldest = minutes->begin();
        for (std::map<std::string, long>::iterator it = minutes->begin(); it != minutes->end(); ++it)
            if (it->second < oldest->second) oldest = it;
        minutes->erase(oldest);
    }
    (*minutes)[key] = minute;
}
}

SensorHistory::SensorHistory(const std::string& path) : path_(path), sequence_(0), initialized_(false) {}
const std::string& SensorHistory::path() const { return path_; }

bool SensorHistory::initialize() {
    int fd = open(path_.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return false;
    struct stat info;
    bool ok = fstat(fd, &info) == 0;
    if (ok && !validHeader(fd, info.st_size)) {
        ok = ftruncate(fd, 0) == 0 && pwrite(fd, header, headerBytes, 0) == headerBytes;
        info.st_size = headerBytes;
    }
    sequence_ = 0;
    lastMinute_.clear();
    lastValue_.clear();
    recordCount_.clear();
    if (ok) {
        unsigned char bytes[recordBytes];
        Sample sample;
        std::map<std::string, unsigned long> lastSequence;
        for (unsigned long slot = 0; slot < capacity && headerBytes + static_cast<off_t>((slot + 1) * recordBytes) <= info.st_size; ++slot) {
            if (pread(fd, bytes, recordBytes, headerBytes + static_cast<off_t>(slot * recordBytes)) == recordBytes &&
                validRecord(bytes, slot, &sample)) {
                if (sample.sequence > sequence_) sequence_ = sample.sequence;
                std::string key = addressKey(bytes + 8, sample.metric);
                ++recordCount_[key];
                if (lastSequence.find(key) == lastSequence.end() || sample.sequence > lastSequence[key]) {
                    lastSequence[key] = sample.sequence;
                    lastValue_[key] = sample.value;
                }
                if (sample.metric != Contact) {
                    long minute = sample.timestamp / 60;
                    rememberMinute(&lastMinute_, key, minute);
                }
            }
        }
    }
    close(fd);
    initialized_ = ok;
    return ok;
}

bool SensorHistory::append(const std::string& address, unsigned char metric, int value, long timestamp) {
    unsigned char ieee[8];
    if (!decodeAddress(address, ieee) || metric < Temperature || metric > Battery ||
        ((metric == Contact || metric == Occupancy) && value != 0 && value != 1) ||
        value < ((metric == Power || metric == Battery) ? 0 : -32768) || value > ((metric == Power || metric == Battery) ? 65535 : 32767) ||
        timestamp <= 0 || static_cast<unsigned long>(timestamp) > 0xffffffffUL) return false;
    if (!initialized_ && !initialize()) return false;
    std::string key = addressKey(ieee, metric);
    if (lastValue_.find(key) != lastValue_.end() && lastValue_[key] == value) return true;
    if (metric != Contact) {
        std::map<std::string, long>::const_iterator previous = lastMinute_.find(key);
        if (previous != lastMinute_.end() && timestamp / 60 <= previous->second) return true;
    }
    if (sequence_ == 0xffffffffUL) {
        int resetFd = open(path_.c_str(), O_RDWR | O_TRUNC);
        if (resetFd < 0) return false;
        bool reset = pwrite(resetFd, header, headerBytes, 0) == headerBytes;
        close(resetFd);
        if (!reset) { initialized_ = false; return false; }
        sequence_ = 0;
        lastMinute_.clear();
        lastValue_.clear();
        recordCount_.clear();
    }
    unsigned char bytes[recordBytes];
    uint32_t next = static_cast<uint32_t>(sequence_ + 1);
    put32(bytes, next);
    put32(bytes + 4, static_cast<uint32_t>(timestamp));
    memcpy(bytes + 8, ieee, 8);
    bytes[16] = metric;
    bytes[17] = static_cast<unsigned char>(value >> 8);
    bytes[18] = static_cast<unsigned char>(value);
    bytes[19] = checksum(bytes);
    int fd = open(path_.c_str(), O_RDWR);
    if (fd < 0) { initialized_ = false; return false; }
    off_t offset = headerBytes + static_cast<off_t>((next - 1) % capacity) * recordBytes;
    unsigned char oldBytes[recordBytes];
    Sample oldSample;
    std::string replacedKey;
    if (pread(fd, oldBytes, recordBytes, offset) == recordBytes &&
        validRecord(oldBytes, (next - 1) % capacity, &oldSample))
        replacedKey = addressKey(oldBytes + 8, oldSample.metric);
    bool ok = pwrite(fd, bytes, recordBytes, offset) == recordBytes;
    close(fd);
    if (!ok) { initialized_ = false; return false; }
    sequence_ = next;
    if (!replacedKey.empty()) {
        std::map<std::string, unsigned long>::iterator existing = recordCount_.find(replacedKey);
        if (existing != recordCount_.end() && --existing->second == 0) {
            recordCount_.erase(existing);
            lastValue_.erase(replacedKey);
            lastMinute_.erase(replacedKey);
        }
    }
    ++recordCount_[key];
    lastValue_[key] = value;
    if (metric != Contact) rememberMinute(&lastMinute_, key, timestamp / 60);
    return true;
}

bool SensorHistory::readJson(const std::string& path, const std::string& address,
                             unsigned long before, std::string* json, std::string* error) {
    unsigned char ieee[8];
    if (!decodeAddress(address, ieee)) { *error = "invalid device address"; return false; }
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0 && errno != ENOENT) { *error = "cannot read sensor history"; return false; }
    std::vector<Sample> samples;
    if (fd >= 0) {
        struct stat info;
        if (fstat(fd, &info) != 0 || !validHeader(fd, info.st_size)) {
            close(fd); *error = "invalid sensor history file"; return false;
        }
        unsigned char bytes[recordBytes];
        Sample sample;
        for (unsigned long slot = 0; slot < capacity && headerBytes + static_cast<off_t>((slot + 1) * recordBytes) <= info.st_size; ++slot) {
            if (pread(fd, bytes, recordBytes, headerBytes + static_cast<off_t>(slot * recordBytes)) == recordBytes &&
                validRecord(bytes, slot, &sample) && memcmp(bytes + 8, ieee, 8) == 0) samples.push_back(sample);
        }
        close(fd);
    }
    std::sort(samples.begin(), samples.end(), newer);
    std::vector<Sample> page;
    bool more = false;
    for (unsigned long i = 0; i < samples.size(); ++i) {
        if (before != 0 && samples[i].sequence >= before) continue;
        if (page.size() == pageLimit) { more = true; break; }
        page.push_back(samples[i]);
    }
    std::ostringstream output;
    output << "{\"samples\":[";
    for (unsigned long i = page.size(); i > 0; --i) {
        const Sample& sample = page[i - 1];
        if (i != page.size()) output << ',';
        output << '[' << sample.sequence << ',' << sample.timestamp << ',' << static_cast<unsigned int>(sample.metric)
               << ',' << sample.value << ']';
    }
    output << "],\"next_before\":" << (more && !page.empty() ? page.back().sequence : 0) << '}';
    *json = output.str();
    return true;
}
