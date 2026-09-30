#include "sensor_history.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
enum {
    fileHeaderBytes = 16, pageHeaderBytes = 16, sampleBytes = 11,
    samplesPerPage = 128, pageBytes = pageHeaderBytes + sampleBytes * samplesPerPage,
    baselinePages = 1472, pageLimit = 2000
};
const unsigned char v2Header[fileHeaderBytes] = {
    'T', 'Z', 'H', '2', 2, fileHeaderBytes, pageHeaderBytes, sampleBytes,
    0, samplesPerPage, 5, 192, 0, 0, 0, 0
};
const unsigned char zeroPageHeader[pageHeaderBytes] = { 0 };

struct Sample {
    uint32_t sequence;
    uint32_t timestamp;
    unsigned char metric;
    int value;
    unsigned char address[8];
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
void put16(unsigned char* bytes, unsigned int value) {
    bytes[0] = static_cast<unsigned char>(value >> 8);
    bytes[1] = static_cast<unsigned char>(value);
}
uint32_t get16(const unsigned char* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 8) | bytes[1];
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
std::string pairKey(const unsigned char* address, unsigned char metric) {
    std::string key(reinterpret_cast<const char*>(address), 8);
    key += static_cast<char>(metric);
    return key;
}
std::string deviceKey(const unsigned char* address) {
    return std::string(reinterpret_cast<const char*>(address), 8);
}
bool metricValueValid(unsigned char metric, int value) {
    if (metric < SensorHistory::Temperature || metric > SensorHistory::Battery) return false;
    if ((metric == SensorHistory::Contact || metric == SensorHistory::Occupancy) && value != 0 && value != 1) return false;
    if (metric == SensorHistory::Power || metric == SensorHistory::Battery) return value >= 0 && value <= 65535;
    return value >= -32768 && value <= 32767;
}
bool unsignedMetric(unsigned char metric) {
    return metric == SensorHistory::Power || metric == SensorHistory::Battery;
}
unsigned char pageHeaderChecksum(const unsigned char* bytes) { return crc8(bytes, 14); }
unsigned char sampleChecksum(const unsigned char* pageHeader, const unsigned char* record) {
    unsigned char input[23];
    memcpy(input, pageHeader, 9);       /* owner address and metric */
    memcpy(input + 9, pageHeader + 10, 4); /* allocation generation */
    memcpy(input + 13, record, 10);    /* sequence, timestamp, value */
    return crc8(input, sizeof(input));
}
bool validV2Header(const unsigned char* bytes, off_t size) {
    unsigned char expected[fileHeaderBytes];
    memcpy(expected, v2Header, sizeof(expected));
    expected[15] = crc8(expected, 15);
    return size >= fileHeaderBytes && memcmp(bytes, expected, sizeof(expected)) == 0;
}
bool validPageHeader(const unsigned char* bytes) {
    if (bytes[9] != 1 || bytes[8] < SensorHistory::Temperature || bytes[8] > SensorHistory::Battery ||
        bytes[15] != 0 || bytes[14] != pageHeaderChecksum(bytes)) return false;
    return true;
}
bool parseSample(const unsigned char* header, const unsigned char* bytes, Sample* sample) {
    if (bytes[10] != sampleChecksum(header, bytes)) return false;
    sample->sequence = get32(bytes);
    sample->timestamp = get32(bytes + 4);
    sample->metric = header[8];
    memcpy(sample->address, header, 8);
    unsigned int raw = get16(bytes + 8);
    sample->value = unsignedMetric(sample->metric) ? static_cast<int>(raw) : static_cast<int16_t>(raw);
    return sample->sequence != 0 && sample->timestamp != 0 && metricValueValid(sample->metric, sample->value);
}
bool newer(const Sample& left, const Sample& right) { return left.sequence > right.sequence; }
void rememberLatest(std::map<std::string, unsigned long>* sequences,
                    std::map<std::string, int>* values,
                    const std::string& key, unsigned long sequence, int value) {
    std::map<std::string, unsigned long>::iterator it = sequences->find(key);
    if (it == sequences->end() || sequence > it->second) {
        (*sequences)[key] = sequence;
        (*values)[key] = value;
    }
}
void rememberMinute(std::map<std::string, long>* minutes, const std::string& key, long minute) {
    std::map<std::string, long>::iterator it = minutes->find(key);
    if (it == minutes->end() || minute > it->second) (*minutes)[key] = minute;
}
std::string buildJson(const std::vector<Sample>& source, unsigned long before,
                      std::string* json) {
    std::vector<Sample> samples = source;
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
        output << '[' << sample.sequence << ',' << sample.timestamp << ','
               << static_cast<unsigned int>(sample.metric) << ',' << sample.value << ']';
    }
    output << "],\"next_before\":" << (more && !page.empty() ? page.back().sequence : 0) << '}';
    *json = output.str();
    return *json;
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
bool readAllAt(int fd, unsigned char* bytes, unsigned long length, off_t offset) {
    unsigned long readBytes = 0;
    while (readBytes < length) {
        ssize_t count = pread(fd, bytes + readBytes, length - readBytes, offset + readBytes);
        if (count <= 0) return false;
        readBytes += static_cast<unsigned long>(count);
    }
    return true;
}
void encodePageHeader(unsigned char* bytes, const unsigned char* address,
                      unsigned char metric, unsigned long generation) {
    memset(bytes, 0, pageHeaderBytes);
    memcpy(bytes, address, 8);
    bytes[8] = metric;
    bytes[9] = 1;
    put32(bytes + 10, static_cast<uint32_t>(generation));
    bytes[14] = pageHeaderChecksum(bytes);
}
void encodeSample(unsigned char* bytes, const unsigned char* header,
                  uint32_t sequence, uint32_t timestamp, int value) {
    put32(bytes, sequence);
    put32(bytes + 4, timestamp);
    put16(bytes + 8, static_cast<unsigned int>(value));
    bytes[10] = sampleChecksum(header, bytes);
}
}

SensorHistory::PageInfo::PageInfo() : pageId(0), allocated(false), metric(0), generation(0), count(0), nextSlot(0),
    minimumSequence(0), maximumSequence(0) {
    memset(address, 0, sizeof(address));
}

SensorHistory::SensorHistory(const std::string& path) : path_(path), sequence_(0), initialized_(false),
    pages_() {
    pthread_mutex_init(&catalogMutex_, 0);
}
SensorHistory::~SensorHistory() { pthread_mutex_destroy(&catalogMutex_); }
const std::string& SensorHistory::path() const { return path_; }

bool SensorHistory::writeNewFileHeader(int fd) {
    unsigned char bytes[fileHeaderBytes];
    memcpy(bytes, v2Header, sizeof(bytes));
    bytes[15] = crc8(bytes, 15);
    return writeAllAt(fd, bytes, sizeof(bytes), 0);
}

bool SensorHistory::initialize(std::string* error) {
    if (initialized_) return true;
    if (error != 0) error->clear();

    int fd = open(path_.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) { if (error != 0) *error = "cannot open sensor history"; return false; }
    struct stat info;
    bool ok = fstat(fd, &info) == 0;
    if (ok && info.st_size == 0) {
        ok = writeNewFileHeader(fd);
        info.st_size = fileHeaderBytes;
    }
    unsigned char fileHeader[fileHeaderBytes];
    if (ok) ok = pread(fd, fileHeader, sizeof(fileHeader), 0) == fileHeaderBytes;
    if (ok && validV2Header(fileHeader, info.st_size)) {
        ok = initializeV2(fd, info.st_size, error);
    } else {
        ok = false;
        if (error != 0) *error = "unrecognized or damaged sensor history header; file preserved";
    }
    if (fd >= 0) close(fd);
    initialized_ = ok;
    return ok;
}

bool SensorHistory::initializeV2(int fd, off_t size, std::string* error) {
    if (size < fileHeaderBytes) { if (error != 0) *error = "truncated V2 history header"; return false; }
    unsigned long pageCount = static_cast<unsigned long>((size - fileHeaderBytes) / pageBytes);
    off_t alignedSize = fileHeaderBytes + static_cast<off_t>(pageCount * pageBytes);
    if (alignedSize != size && ftruncate(fd, alignedSize) != 0) {
        if (error != 0) *error = "cannot repair incomplete trailing history page";
        return false;
    }
    pages_.clear();
    pairPages_.clear();
    inactiveDevices_.clear();
    lastMinute_.clear();
    lastValue_.clear();
    recordCount_.clear();
    sequence_ = 0;
    pages_.resize(pageCount);
    std::map<std::string, unsigned long> latestSequence;
    for (unsigned long pageId = 0; pageId < pageCount; ++pageId) {
        unsigned char page[pageBytes];
        off_t offset = fileHeaderBytes + static_cast<off_t>(pageId * pageBytes);
        if (!readAllAt(fd, page, sizeof(page), offset)) continue;
        unsigned char* pageHeader = page;
        PageInfo& info = pages_[pageId];
        info.pageId = pageId;
        if (!validPageHeader(pageHeader)) continue;
        info.allocated = true;
        memcpy(info.address, pageHeader, 8);
        info.metric = pageHeader[8];
        info.generation = get32(pageHeader + 10);
        std::string key = pairKey(info.address, info.metric);
        pairPages_[key].push_back(pageId);
        unsigned char* records = page + pageHeaderBytes;
        for (unsigned int slot = 0; slot < samplesPerPage; ++slot) {
            Sample sample;
            if (!parseSample(pageHeader, records + slot * sampleBytes, &sample)) {
                if (info.nextSlot == slot) info.nextSlot = slot;
                continue;
            }
            ++info.count;
            if (info.count == 1 || sample.sequence < info.minimumSequence) info.minimumSequence = sample.sequence;
            if (sample.sequence > info.maximumSequence) info.maximumSequence = sample.sequence;
            if (sample.sequence > sequence_) sequence_ = sample.sequence;
            ++recordCount_[key];
            rememberLatest(&latestSequence, &lastValue_, key, sample.sequence, sample.value);
            if (sample.metric != Contact) rememberMinute(&lastMinute_, key, sample.timestamp / 60);
        }
        while (info.nextSlot < samplesPerPage) {
            Sample nextSample;
            if (!parseSample(pageHeader, records + info.nextSlot * sampleBytes, &nextSample)) break;
            ++info.nextSlot;
        }
        if (info.nextSlot >= samplesPerPage) info.nextSlot = samplesPerPage;
    }
    return true;
}

bool SensorHistory::setDevicePresent(const std::string& address, bool present) {
    unsigned char ieee[8];
    if (!decodeAddress(address, ieee)) return false;
    pthread_mutex_lock(&catalogMutex_);
    if (present) inactiveDevices_.erase(deviceKey(ieee));
    else inactiveDevices_.insert(deviceKey(ieee));
    pthread_mutex_unlock(&catalogMutex_);
    return true;
}
void SensorHistory::setActiveDevices(const std::vector<std::string>& addresses) {
    std::set<std::string> active;
    for (unsigned long i = 0; i < addresses.size(); ++i) {
        unsigned char ieee[8];
        if (decodeAddress(addresses[i], ieee)) active.insert(deviceKey(ieee));
    }
    pthread_mutex_lock(&catalogMutex_);
    inactiveDevices_.clear();
    for (std::map<std::string, std::vector<unsigned long> >::const_iterator it = pairPages_.begin(); it != pairPages_.end(); ++it) {
        std::string device = it->first.substr(0, 8);
        if (active.find(device) == active.end()) inactiveDevices_.insert(device);
    }
    pthread_mutex_unlock(&catalogMutex_);
}

void SensorHistory::rebuildPairState(const std::string& key) {
    recordCount_.erase(key); lastValue_.erase(key); lastMinute_.erase(key);
    std::map<std::string, std::vector<unsigned long> >::const_iterator pageIt = pairPages_.find(key);
    if (pageIt == pairPages_.end()) return;
    int latestValue = 0;
    unsigned long latestSequence = 0;
    long latestMinute = 0;
    int fd = open(path_.c_str(), O_RDONLY);
    if (fd < 0) return;
    for (unsigned long i = 0; i < pageIt->second.size(); ++i) {
        unsigned long pageId = pageIt->second[i];
        if (pageId >= pages_.size() || !pages_[pageId].allocated) continue;
        unsigned char page[pageBytes];
        if (!readAllAt(fd, page, sizeof(page), fileHeaderBytes + static_cast<off_t>(pageId * pageBytes)) || !validPageHeader(page)) continue;
        for (unsigned int slot = 0; slot < samplesPerPage; ++slot) {
            Sample sample;
            if (!parseSample(page, page + pageHeaderBytes + slot * sampleBytes, &sample)) continue;
            ++recordCount_[key];
            if (sample.sequence > latestSequence) { latestSequence = sample.sequence; latestValue = sample.value; }
            if (sample.metric != Contact && static_cast<long>(sample.timestamp / 60) > latestMinute)
                latestMinute = sample.timestamp / 60;
        }
    }
    close(fd);
    if (latestSequence != 0) lastValue_[key] = latestValue;
    if (latestMinute != 0) lastMinute_[key] = latestMinute;
}

bool SensorHistory::allocatePage(const unsigned char* address, unsigned char metric,
                                 unsigned long* pageId) {
    std::string key = pairKey(address, metric);
    unsigned long selected = pages_.size();
    bool reassign = false;
    for (unsigned long i = 0; i < pages_.size(); ++i) {
        if (!pages_[i].allocated) { selected = i; reassign = true; break; }
    }
    if (selected == pages_.size() && pages_.size() < baselinePages) {
        selected = pages_.size();
        pages_.push_back(PageInfo());
        reassign = true;
    }
    if (selected == pages_.size()) {
        unsigned long oldestSequence = 0xffffffffUL;
        for (unsigned long i = 0; i < pages_.size(); ++i) {
            if (!pages_[i].allocated) { selected = i; reassign = true; break; }
            std::string ownerKey = pairKey(pages_[i].address, pages_[i].metric);
            std::string ownerDevice = deviceKey(pages_[i].address);
            std::map<std::string, std::vector<unsigned long> >::const_iterator ownerPages = pairPages_.find(ownerKey);
            bool inactive = inactiveDevices_.find(ownerDevice) != inactiveDevices_.end();
            bool surplus = ownerPages != pairPages_.end() && ownerPages->second.size() > 1;
            if ((inactive || surplus) && pages_[i].maximumSequence <= oldestSequence) {
                oldestSequence = pages_[i].maximumSequence;
                selected = i;
                reassign = true;
            }
        }
    }
    if (!reassign) {
        std::map<std::string, std::vector<unsigned long> >::iterator existing = pairPages_.find(key);
        if (existing != pairPages_.end() && existing->second.size() == 1) {
            *pageId = existing->second[0];
            return true; /* protected single page is reused as a local ring */
        }
        selected = pages_.size(); /* overflow only when every baseline page is protected */
        pages_.push_back(PageInfo());
        reassign = true;
    }

    int fd = open(path_.c_str(), O_RDWR);
    if (fd < 0) return false;
    PageInfo previous = selected < pages_.size() ? pages_[selected] : PageInfo();
    unsigned long generation = previous.generation + 1;
    if (generation == 0) generation = 1;
    unsigned char zero[pageBytes]; memset(zero, 0, sizeof(zero));
    off_t offset = fileHeaderBytes + static_cast<off_t>(selected * pageBytes);
    struct stat fileInfo;
    bool ok = fstat(fd, &fileInfo) == 0;
    if (ok && fileInfo.st_size < offset + pageBytes) ok = ftruncate(fd, offset + pageBytes) == 0;
    ok = ok && writeAllAt(fd, zeroPageHeader, pageHeaderBytes, offset) &&
         writeAllAt(fd, zero + pageHeaderBytes, pageBytes - pageHeaderBytes, offset + pageHeaderBytes);
    unsigned char newHeader[pageHeaderBytes];
    encodePageHeader(newHeader, address, metric, generation);
    if (ok) ok = writeAllAt(fd, newHeader, sizeof(newHeader), offset);
    close(fd);
    if (!ok) return false;

    if (previous.allocated) {
        std::string oldKey = pairKey(previous.address, previous.metric);
        std::vector<unsigned long>& ids = pairPages_[oldKey];
        ids.erase(std::remove(ids.begin(), ids.end(), selected), ids.end());
        if (ids.empty()) pairPages_.erase(oldKey);
        rebuildPairState(oldKey);
    }
    PageInfo info;
    info.pageId = selected;
    info.allocated = true;
    memcpy(info.address, address, 8);
    info.metric = metric;
    info.generation = generation;
    if (selected >= pages_.size()) pages_.resize(selected + 1);
    pages_[selected] = info;
    pairPages_[key].push_back(selected);
    *pageId = selected;
    return true;
}

bool SensorHistory::append(const std::string& address, unsigned char metric, int value, long timestamp) {
    unsigned char ieee[8];
    if (!decodeAddress(address, ieee) || !metricValueValid(metric, value) || timestamp <= 0 ||
        static_cast<unsigned long>(timestamp) > 0xffffffffUL) return false;
    if (!initialized_ && !initialize()) return false;
    pthread_mutex_lock(&catalogMutex_);
    std::string key = pairKey(ieee, metric);
    if (lastValue_.find(key) != lastValue_.end() && lastValue_[key] == value) {
        pthread_mutex_unlock(&catalogMutex_); return true;
    }
    if (metric != Contact && lastMinute_.find(key) != lastMinute_.end() && timestamp / 60 <= lastMinute_[key]) {
        pthread_mutex_unlock(&catalogMutex_); return true;
    }
    if (sequence_ == 0xffffffffUL) {
        int resetFd = open(path_.c_str(), O_RDWR | O_TRUNC);
        bool reset = resetFd >= 0 && writeNewFileHeader(resetFd);
        if (resetFd >= 0) close(resetFd);
        if (!reset) { pthread_mutex_unlock(&catalogMutex_); return false; }
        sequence_ = 0; pages_.clear(); pairPages_.clear(); lastMinute_.clear(); lastValue_.clear(); recordCount_.clear();
    }
    std::vector<unsigned long>* pairIds = &pairPages_[key];
    unsigned long selected = pages_.size();
    for (unsigned long i = 0; i < pairIds->size(); ++i)
        if (pages_[(*pairIds)[i]].count < samplesPerPage && (*pairIds)[i] < selected) selected = (*pairIds)[i];
    bool localOverwrite = false;
    if (selected == pages_.size()) {
        if (!allocatePage(ieee, metric, &selected)) { pthread_mutex_unlock(&catalogMutex_); return false; }
        if (pages_[selected].count >= samplesPerPage) localOverwrite = true;
    }
    PageInfo& info = pages_[selected];
    int fd = open(path_.c_str(), O_RDWR);
    if (fd < 0) { pthread_mutex_unlock(&catalogMutex_); return false; }
    off_t pageOffset = fileHeaderBytes + static_cast<off_t>(selected * pageBytes);
    unsigned char pageHeader[pageHeaderBytes];
    bool ok = readAllAt(fd, pageHeader, sizeof(pageHeader), pageOffset) && validPageHeader(pageHeader);
    unsigned int slot = info.nextSlot;
    unsigned char page[pageBytes];
    if (ok && localOverwrite) {
        ok = readAllAt(fd, page, sizeof(page), pageOffset);
        if (ok) {
            unsigned long oldest = 0xffffffffUL;
            slot = 0;
            for (unsigned int i = 0; i < samplesPerPage; ++i) {
                Sample old;
                if (parseSample(pageHeader, page + pageHeaderBytes + i * sampleBytes, &old) && old.sequence < oldest) {
                    oldest = old.sequence; slot = i;
                }
            }
            if (oldest == 0xffffffffUL) ok = false;
        }
    }
    unsigned char record[sampleBytes];
    uint32_t next = static_cast<uint32_t>(sequence_ + 1);
    encodeSample(record, pageHeader, next, static_cast<uint32_t>(timestamp), value);
    if (ok) ok = writeAllAt(fd, record, sizeof(record), pageOffset + pageHeaderBytes + static_cast<off_t>(slot * sampleBytes));
    close(fd);
    if (!ok) { pthread_mutex_unlock(&catalogMutex_); return false; }
    sequence_ = next;
    if (!localOverwrite) ++info.count;
    info.nextSlot = samplesPerPage;
    if (info.count < samplesPerPage) {
        unsigned char verify[sampleBytes];
        int scanFd = open(path_.c_str(), O_RDONLY);
        if (scanFd >= 0) {
            for (unsigned int i = 0; i < samplesPerPage; ++i) {
                if (pread(scanFd, verify, sizeof(verify), pageOffset + pageHeaderBytes + static_cast<off_t>(i * sampleBytes)) == sampleBytes) {
                    Sample unused;
                    if (!parseSample(pageHeader, verify, &unused)) { info.nextSlot = i; break; }
                }
            }
            close(scanFd);
        }
    }
    if (localOverwrite) {
        info.minimumSequence = 0; info.maximumSequence = 0;
        int scanFd = open(path_.c_str(), O_RDONLY);
        if (scanFd >= 0 && readAllAt(scanFd, page, sizeof(page), pageOffset)) {
            for (unsigned int i = 0; i < samplesPerPage; ++i) {
                Sample current;
                if (parseSample(pageHeader, page + pageHeaderBytes + i * sampleBytes, &current)) {
                    if (info.minimumSequence == 0 || current.sequence < info.minimumSequence) info.minimumSequence = current.sequence;
                    if (current.sequence > info.maximumSequence) info.maximumSequence = current.sequence;
                }
            }
        }
        if (scanFd >= 0) close(scanFd);
    } else {
        if (info.count == 1 || info.minimumSequence == 0 || next < info.minimumSequence) info.minimumSequence = next;
        if (next > info.maximumSequence) info.maximumSequence = next;
    }
    ++recordCount_[key]; lastValue_[key] = value;
    if (metric != Contact) rememberMinute(&lastMinute_, key, timestamp / 60);
    pthread_mutex_unlock(&catalogMutex_);
    return true;
}

bool SensorHistory::readV2Json(const std::string& address, unsigned long before,
        std::string* json, std::string* error, ReadStats* stats) const {
    unsigned char ieee[8];
    if (!decodeAddress(address, ieee)) { *error = "invalid device address"; return false; }
    std::vector<PageInfo> snapshot;
    pthread_mutex_lock(&catalogMutex_);
    if (!initialized_) {
        pthread_mutex_unlock(&catalogMutex_); *error = "sensor history catalog is unavailable"; return false;
    }
    for (unsigned char metric = Temperature; metric <= Battery; ++metric) {
        std::string key = pairKey(ieee, metric);
        std::map<std::string, std::vector<unsigned long> >::const_iterator found = pairPages_.find(key);
        if (found != pairPages_.end())
            for (unsigned long i = 0; i < found->second.size(); ++i)
                if (found->second[i] < pages_.size()) snapshot.push_back(pages_[found->second[i]]);
    }
    pthread_mutex_unlock(&catalogMutex_);
    int fd = open(path_.c_str(), O_RDONLY);
    if (fd < 0) { if (errno == ENOENT) { *json = "{\"samples\":[],\"next_before\":0}"; return true; }
        *error = "cannot read sensor history"; return false; }
    struct stat fileInfo;
    unsigned char globalHeader[fileHeaderBytes];
    if (fstat(fd, &fileInfo) != 0 || pread(fd, globalHeader, sizeof(globalHeader), 0) != fileHeaderBytes ||
        !validV2Header(globalHeader, fileInfo.st_size)) {
        close(fd); *error = "invalid V2 sensor history file"; return false;
    }
    std::vector<Sample> samples;
    for (unsigned long i = 0; i < snapshot.size(); ++i) {
        const PageInfo& indexed = snapshot[i];
        unsigned char page[pageBytes];
        off_t offset = fileHeaderBytes + static_cast<off_t>(indexed.pageId * pageBytes);
        if (!readAllAt(fd, page, sizeof(page), offset) || !validPageHeader(page) ||
            memcmp(page, indexed.address, 8) != 0 || page[8] != indexed.metric || get32(page + 10) != indexed.generation) continue;
        if (stats != 0) { ++stats->pagesRead; stats->bytesRead += pageBytes; stats->recordsChecked += samplesPerPage; }
        for (unsigned int slot = 0; slot < samplesPerPage; ++slot) {
            Sample sample;
            if (parseSample(page, page + pageHeaderBytes + slot * sampleBytes, &sample)) samples.push_back(sample);
        }
    }
    close(fd);
    buildJson(samples, before, json);
    return true;
}

bool SensorHistory::readForDevice(const std::string& address, unsigned long before,
        std::string* json, std::string* error, ReadStats* stats) const {
    return readV2Json(address, before, json, error, stats);
}

bool SensorHistory::readJson(const std::string& path, const std::string& address,
        unsigned long before, std::string* json, std::string* error, ReadStats* stats) {
    SensorHistory reader(path);
    if (!reader.initialize(error)) return false;
    return reader.readForDevice(address, before, json, error, stats);
}
