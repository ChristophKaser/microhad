#include "sensor_history.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MICROHAD_TESTING
void (*sensorHistorySnapshotHook)(void*) = 0;
void* sensorHistorySnapshotHookData = 0;
#endif

namespace {
enum { fileHeaderBytes = 16, pageHeaderBytes = 85, recordBytes = 13, recordsPerPage = 151,
       pageBytes = 2048, pageLimit = 1023, apiPageLimit = 2000, maximumMetric = 19,
       maximumFileBytes = fileHeaderBytes + pageLimit * pageBytes };
const unsigned char fileTemplate[fileHeaderBytes] = {
    'T', 'Z', 'H', '3', 3, fileHeaderBytes, pageHeaderBytes, recordBytes,
    0, recordsPerPage, 0x03, 0xff, 0, 0, 0, 0
};

void put32(unsigned char* b, uint32_t v) {
    b[0] = static_cast<unsigned char>(v >> 24); b[1] = static_cast<unsigned char>(v >> 16);
    b[2] = static_cast<unsigned char>(v >> 8); b[3] = static_cast<unsigned char>(v);
}
uint32_t get32(const unsigned char* b) {
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
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
bool plainText(const std::string& s, unsigned long maximum, bool allowEmpty) {
    if ((!allowEmpty && s.empty()) || s.size() > maximum) return false;
    for (unsigned long i = 0; i < s.size(); ++i)
        if (static_cast<unsigned char>(s[i]) < 0x20 || static_cast<unsigned char>(s[i]) > 0x7e) return false;
    return true;
}
std::string pairKey(const std::string& id, unsigned char metric) {
    std::string result = id; result += '\0'; result += static_cast<char>(metric); return result;
}
std::string jsonString(const std::string& value) {
    std::string result = "\"";
    for (unsigned long i = 0; i < value.size(); ++i) {
        if (value[i] == '"' || value[i] == '\\') result += '\\';
        result += value[i];
    }
    result += '"'; return result;
}
bool olderRecord(const SensorHistory::ValueRecord& a, const SensorHistory::ValueRecord& b) {
    return a.sequence < b.sequence;
}
bool newerRecord(const SensorHistory::ValueRecord& a, const SensorHistory::ValueRecord& b) {
    return a.sequence > b.sequence;
}
struct PageOrder {
    const std::vector<SensorHistory::Page>* pages;
    explicit PageOrder(const std::vector<SensorHistory::Page>* p) : pages(p) {}
    bool operator()(unsigned int a, unsigned int b) const { return (*pages)[a].newest < (*pages)[b].newest; }
};
struct ReadPage {
    unsigned int index;
    SensorHistory::Page page;
    ReadPage(unsigned int i, const SensorHistory::Page& p) : index(i), page(p) {}
};
bool newestPageFirst(const ReadPage& a, const ReadPage& b) { return a.page.newest > b.page.newest; }
bool fileHeaderValid(const unsigned char* bytes) {
    unsigned char expected[fileHeaderBytes]; memcpy(expected, fileTemplate, sizeof(expected));
    expected[15] = crc8(expected, 15); return memcmp(bytes, expected, sizeof(expected)) == 0;
}
const char* unitForMetric(unsigned char metric) {
    switch (metric) {
    case SensorHistory::Temperature: return "celsius";
    case SensorHistory::Humidity: case SensorHistory::Battery: case SensorHistory::Brightness: return "percent";
    case SensorHistory::Power: return "watts";
    case SensorHistory::CarbonDioxide: case SensorHistory::Pm25Ppm: return "ppm";
    case SensorHistory::Pm25: return "ug/m3";
    case SensorHistory::CarbonDioxideMass: return "ug/m3";
    case SensorHistory::CarbonDioxideParticles: case SensorHistory::Pm25Particles: return "particles/m3";
    case SensorHistory::CarbonDioxideBecquerels: case SensorHistory::Pm25Becquerels: return "Bq/m3";
    case SensorHistory::CarbonDioxideDecibels: case SensorHistory::Pm25Decibels: return "dB/m";
    default: return "";
    }
}
bool metricDiscrete(unsigned char metric) {
    return metric == SensorHistory::Contact || metric == SensorHistory::Occupancy ||
           metric == SensorHistory::AirQuality || metric == SensorHistory::Light;
}
bool normalizeValue(unsigned char inputMetric, const std::string& value, const std::string& unit,
                    unsigned char* metric, float* number, std::string* text) {
    if (!plainText(value, 24, false) || !plainText(unit, 16, true)) return false;
    if (inputMetric == SensorHistory::AirQuality) {
        static const char* const qualities[] = {"good", "fair", "moderate", "poor", "very_poor", "extremely_poor"};
        if (!unit.empty()) return false;
        for (int i = 0; i < 6; ++i) if (value == qualities[i]) { *metric = inputMetric; *number = static_cast<float>(i); *text = value; return true; }
        return false;
    }
    if (inputMetric == SensorHistory::Contact || inputMetric == SensorHistory::Occupancy || inputMetric == SensorHistory::Light) {
        if (!unit.empty() || (value != "0" && value != "1")) return false;
        *metric = inputMetric; *number = value == "1" ? 1.0f : 0.0f; text->clear(); return true;
    }
    char* end = 0; double v = std::strtod(value.c_str(), &end);
    if (end != value.c_str() + value.size() || !std::isfinite(v)) return false;
    unsigned char m = inputMetric;
    double factor = 1.0;
    if (inputMetric == SensorHistory::CarbonDioxide || inputMetric == SensorHistory::Pm25) {
        const bool co2 = inputMetric == SensorHistory::CarbonDioxide;
        if (unit == "ppm") { m = co2 ? SensorHistory::CarbonDioxide : SensorHistory::Pm25Ppm; }
        else if (unit == "ppb") { m = co2 ? SensorHistory::CarbonDioxide : SensorHistory::Pm25Ppm; factor = 0.001; }
        else if (unit == "ppt") { m = co2 ? SensorHistory::CarbonDioxide : SensorHistory::Pm25Ppm; factor = 0.000001; }
        else if (unit == "mg/m3") { m = co2 ? SensorHistory::CarbonDioxideMass : SensorHistory::Pm25; factor = 1000.0; }
        else if (unit == "ug/m3") { m = co2 ? SensorHistory::CarbonDioxideMass : SensorHistory::Pm25; }
        else if (unit == "ng/m3") { m = co2 ? SensorHistory::CarbonDioxideMass : SensorHistory::Pm25; factor = 0.001; }
        else if (unit == "particles/m3" || unit == "particles/ft3") {
            m = co2 ? SensorHistory::CarbonDioxideParticles : SensorHistory::Pm25Particles;
            if (unit == "particles/ft3") factor = 35.3146667215;
        } else if (unit == "Bq/m3") m = co2 ? SensorHistory::CarbonDioxideBecquerels : SensorHistory::Pm25Becquerels;
        else if (unit == "dB/m") m = co2 ? SensorHistory::CarbonDioxideDecibels : SensorHistory::Pm25Decibels;
        else return false;
        if (v < 0) return false;
    } else {
        if (inputMetric == SensorHistory::Temperature && (unit != "celsius" || v < -327.68 || v > 327.67)) return false;
        if (inputMetric == SensorHistory::Humidity && (unit != "percent" || v < 0 || v > 100)) return false;
        if (inputMetric == SensorHistory::Battery && (unit != "percent" || v < 0 || v > 100)) return false;
        if (inputMetric == SensorHistory::Power && (unit != "watts" || v < 0 || v > 6553.5)) return false;
        if (inputMetric == SensorHistory::Brightness && (unit != "percent" || v < 0 || v > 100)) return false;
        if (inputMetric < SensorHistory::Temperature || inputMetric > SensorHistory::Brightness) return false;
    }
    v *= factor;
    if (!std::isfinite(v) || std::fabs(v) > 3.402823466e38) return false;
    *metric = m; *number = static_cast<float>(v); text->clear(); return true;
}
unsigned char recordCrc(const unsigned char* pageHeader, const unsigned char* record) {
    unsigned char data[71 + 4 + 12];
    memcpy(data, pageHeader, 71); memcpy(data + 71, pageHeader + 3, 4); memcpy(data + 75, record, 12);
    return crc8(data, sizeof(data));
}
void encodeFloat(unsigned char* b, float value) {
    uint32_t bits = 0; memcpy(&bits, &value, sizeof(bits)); put32(b, bits);
}
float decodeFloat(const unsigned char* b) {
    uint32_t bits = get32(b); float value = 0; memcpy(&value, &bits, sizeof(value)); return value;
}
std::string numberText(float value) {
    std::ostringstream out; out << std::setprecision(9) << value; return out.str();
}
std::string qualityText(float value) {
    static const char* const qualities[] = {"good", "fair", "moderate", "poor", "very_poor", "extremely_poor"};
    int index = static_cast<int>(value); return index >= 0 && index < 6 ? qualities[index] : "";
}
bool pageHeaderValid(const unsigned char* h, std::string* id, unsigned char* metric, uint32_t* generation) {
    if (h[2] != 1 || h[0] == 0 || h[0] > 64 || h[1] < 1 || h[1] > maximumMetric || h[84] != crc8(h, 84)) return false;
    for (int i = 71; i <= 83; ++i) if (h[i] != 0) return false;
    id->assign(reinterpret_cast<const char*>(h + 7), h[0]);
    *metric = h[1]; *generation = get32(h + 3);
    return plainText(*id, 64, false) && *generation != 0;
}
void makePageHeader(unsigned char* h, const std::string& id, unsigned char metric, uint32_t generation) {
    memset(h, 0, pageHeaderBytes); h[0] = static_cast<unsigned char>(id.size()); h[1] = metric; h[2] = 1;
    put32(h + 3, generation); memcpy(h + 7, id.data(), id.size()); h[84] = crc8(h, 84);
}
}

SensorHistory::ValueJournal::ValueJournal() : sequence(0), initialized(false) { pthread_mutex_init(&mutex, 0); }
SensorHistory::ValueJournal::~ValueJournal() { pthread_mutex_destroy(&mutex); }
SensorHistory::SensorHistory(const std::string& path) : obsoleteHistoryPath_(path) { values_.path = path + ".values"; }
SensorHistory::~SensorHistory() {}
const std::string& SensorHistory::path() const { return values_.path; }

bool SensorHistory::initialize(std::string* error) {
    if (error) error->clear();
    return initializeValues(error);
}
bool SensorHistory::initializeValues(std::string* error) {
    pthread_mutex_lock(&values_.mutex);
    if (values_.initialized) { pthread_mutex_unlock(&values_.mutex); return true; }
    unlink(obsoleteHistoryPath_.c_str());
    int fd = open(values_.path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) { if (error) *error = "cannot open paged history"; pthread_mutex_unlock(&values_.mutex); return false; }
    struct stat st; bool ok = fstat(fd, &st) == 0;
    unsigned char h[fileHeaderBytes];
    if (ok && st.st_size == 0) {
        memcpy(h, fileTemplate, sizeof(h)); h[15] = crc8(h, 15);
        ok = writeAllAt(fd, h, sizeof(h), 0); st.st_size = fileHeaderBytes;
    }
    if (ok && st.st_size >= 4 && pread(fd, h, sizeof(h), 0) == fileHeaderBytes &&
        memcmp(h, "TUV1", 4) == 0) {
        /* The former flat journal is intentionally reset; v3 does not migrate it. */
        ok = ftruncate(fd, 0) == 0;
        if (ok) {
            memcpy(h, fileTemplate, sizeof(h)); h[15] = crc8(h, 15);
            ok = writeAllAt(fd, h, sizeof(h), 0); st.st_size = fileHeaderBytes;
        }
    }
    if (ok) ok = pread(fd, h, sizeof(h), 0) == fileHeaderBytes && fileHeaderValid(h);
    if (ok && st.st_size > maximumFileBytes) ok = false;
    if (!ok) { if (error) *error = "invalid paged history header; file preserved"; close(fd); pthread_mutex_unlock(&values_.mutex); return false; }

    values_.pages.assign(pageLimit, Page()); values_.pairPages.clear(); values_.lastValues.clear(); values_.sequence = 0;
    for (unsigned int i = 0; i < pageLimit; ++i) {
        off_t base = fileHeaderBytes + static_cast<off_t>(i) * pageBytes;
        if (base + pageHeaderBytes > st.st_size) break;
        unsigned char ph[pageHeaderBytes];
        if (pread(fd, ph, sizeof(ph), base) != static_cast<ssize_t>(sizeof(ph))) continue;
        std::string id; unsigned char metric = 0; uint32_t generation = 0;
        if (!pageHeaderValid(ph, &id, &metric, &generation)) continue;
        Page& page = values_.pages[i]; page.allocated = true; page.id = id; page.metric = metric; page.generation = generation;
        for (unsigned int r = 0; r < recordsPerPage; ++r) {
            unsigned char rec[recordBytes]; off_t at = base + pageHeaderBytes + static_cast<off_t>(r) * recordBytes;
            if (at + recordBytes > st.st_size || pread(fd, rec, sizeof(rec), at) != static_cast<ssize_t>(sizeof(rec))) break;
            if (rec[12] != recordCrc(ph, rec)) break;
            uint32_t seq = get32(rec), timestamp = get32(rec + 4); float v = decodeFloat(rec + 8);
            if (seq == 0 || timestamp == 0 || !std::isfinite(v)) break;
            std::string text = metric == AirQuality ? qualityText(v) : numberText(v);
            if (text.empty()) break;
            ++page.recordCount;
            if (page.oldest == 0 || seq < page.oldest) page.oldest = seq;
            if (seq > page.newest) page.newest = seq;
            if (seq > page.lastSequence) {
                page.lastSequence = seq; page.lastValue = v; page.lastTimestamp = timestamp;
            }
            const std::string key = pairKey(id, metric);
            std::map<std::string, LastValue>::iterator previous = values_.lastValues.find(key);
            if (previous == values_.lastValues.end() || seq > previous->second.sequence)
                values_.lastValues[key] = LastValue(seq, timestamp, v);
            if (seq > values_.sequence) values_.sequence = seq;
        }
        if (page.recordCount != 0) {
            const std::string key = pairKey(id, metric); values_.pairPages[key].push_back(i);
        } else page.allocated = false;
    }
    for (std::map<std::string, std::vector<unsigned int> >::iterator pair = values_.pairPages.begin();
         pair != values_.pairPages.end(); ++pair)
        std::sort(pair->second.begin(), pair->second.end(), PageOrder(&values_.pages));
    close(fd); values_.initialized = true; pthread_mutex_unlock(&values_.mutex); return true;
}

bool SensorHistory::appendValue(const std::string& deviceId, unsigned char metric, const std::string& value,
                               const std::string& unit, long timestamp) {
    if (!initializeValues(0)) return false;
    pthread_mutex_lock(&values_.mutex);
    bool result = appendValueLocked(deviceId, metric, value, unit, timestamp);
    pthread_mutex_unlock(&values_.mutex); return result;
}
bool SensorHistory::appendValueLocked(const std::string& deviceId, unsigned char inputMetric, const std::string& value,
                                      const std::string& unit, long timestamp) {
    if (!values_.initialized || !plainText(deviceId, 64, false) || timestamp <= 0 ||
        static_cast<unsigned long>(timestamp) > 0xffffffffUL) return false;
    unsigned char metric = 0; float numeric = 0; std::string text;
    if (!normalizeValue(inputMetric, value, unit, &metric, &numeric, &text)) return false;
    if (values_.sequence >= 0xffffffffUL) return false;
    const std::string key = pairKey(deviceId, metric);
    std::vector<unsigned int>& owned = values_.pairPages[key];
    Page* page = owned.empty() ? 0 : &values_.pages[owned.back()];
    std::map<std::string, LastValue>::const_iterator last = values_.lastValues.find(key);
    if (last != values_.lastValues.end() && last->second.value == numeric) return true;
    if (!metricDiscrete(metric) && last != values_.lastValues.end() &&
        static_cast<unsigned long>(timestamp) / 60 <= last->second.timestamp / 60) return true;
    if (page != 0 && page->recordCount >= recordsPerPage) page = 0;

    int fd = open(values_.path.c_str(), O_RDWR);
    if (fd < 0) return false;
    unsigned int pageIndex = 0;
    unsigned long nextGeneration = 1;
    if (page == 0) {
        bool found = false;
        for (unsigned int i = 0; i < values_.pages.size(); ++i) if (!values_.pages[i].allocated) { pageIndex = i; found = true; break; }
        if (!found) {
            unsigned long oldest = 0xffffffffUL; int victim = -1;
            for (unsigned int i = 0; i < values_.pages.size(); ++i) {
                Page& candidate = values_.pages[i];
                std::string candidateKey = pairKey(candidate.id, candidate.metric);
                std::vector<unsigned int>& siblings = values_.pairPages[candidateKey];
                if (candidateKey == key && siblings.size() <= 1) continue;
                if (candidate.newest < oldest) { oldest = candidate.newest; victim = static_cast<int>(i); }
            }
            if (victim < 0) { close(fd); return false; }
            pageIndex = static_cast<unsigned int>(victim);
            Page& evicted = values_.pages[pageIndex];
            nextGeneration = evicted.generation >= 0xffffffffUL ? 1 : evicted.generation + 1;
            std::vector<unsigned int>& siblings = values_.pairPages[pairKey(evicted.id, evicted.metric)];
            siblings.erase(std::remove(siblings.begin(), siblings.end(), pageIndex), siblings.end());
            if (siblings.empty()) {
                values_.pairPages.erase(pairKey(evicted.id, evicted.metric));
                values_.lastValues.erase(pairKey(evicted.id, evicted.metric));
            } else {
                unsigned long latest = 0;
                LastValue newest;
                for (unsigned long s = 0; s < siblings.size(); ++s) {
                    const Page& remaining = values_.pages[siblings[s]];
                    if (remaining.lastSequence > latest) {
                        latest = remaining.lastSequence;
                        newest = LastValue(remaining.lastSequence, remaining.lastTimestamp, remaining.lastValue);
                    }
                }
                values_.lastValues[pairKey(evicted.id, evicted.metric)] = newest;
            }
            evicted = Page();
        }
        Page& allocated = values_.pages[pageIndex]; allocated.allocated = true; allocated.id = deviceId; allocated.metric = metric;
        allocated.generation = nextGeneration;
        unsigned char ph[pageHeaderBytes]; makePageHeader(ph, deviceId, metric, allocated.generation);
        off_t base = fileHeaderBytes + static_cast<off_t>(pageIndex) * pageBytes;
        struct stat fileInfo;
        bool extended = fstat(fd, &fileInfo) == 0 &&
            (fileInfo.st_size >= base + pageBytes || ftruncate(fd, base + pageBytes) == 0);
        /* Invalidate ownership, erase old records, then publish the new header.
           CRC-8 identity collisions must never resurrect a previous owner's data. */
        unsigned char cleared[pageBytes]; memset(cleared, 0, sizeof(cleared));
        if (!extended || !writeAllAt(fd, cleared, pageHeaderBytes, base) ||
            !writeAllAt(fd, cleared + pageHeaderBytes, pageBytes - pageHeaderBytes, base + pageHeaderBytes) ||
            !writeAllAt(fd, ph, sizeof(ph), base)) { allocated = Page(); close(fd); return false; }
        owned.push_back(pageIndex); page = &allocated;
    } else pageIndex = static_cast<unsigned int>(page - &values_.pages[0]);

    unsigned long sequence = values_.sequence + 1;
    unsigned char rec[recordBytes]; put32(rec, static_cast<uint32_t>(sequence)); put32(rec + 4, static_cast<uint32_t>(timestamp));
    encodeFloat(rec + 8, numeric);
    unsigned char ph[pageHeaderBytes]; off_t base = fileHeaderBytes + static_cast<off_t>(pageIndex) * pageBytes;
    if (pread(fd, ph, sizeof(ph), base) != static_cast<ssize_t>(sizeof(ph))) { close(fd); return false; }
    rec[12] = recordCrc(ph, rec);
    unsigned int slot = page->recordCount;
    if (slot >= recordsPerPage) { close(fd); return false; }
    off_t offset = base + pageHeaderBytes + static_cast<off_t>(slot) * recordBytes;
    bool ok = writeAllAt(fd, rec, sizeof(rec), offset); close(fd); if (!ok) return false;
    ++page->recordCount; page->lastValue = numeric;
    if (page->oldest == 0) page->oldest = sequence;
    page->lastTimestamp = static_cast<uint32_t>(timestamp); page->newest = sequence; page->lastSequence = sequence;
    values_.lastValues[key] = LastValue(sequence, static_cast<unsigned long>(timestamp), numeric);
    values_.sequence = sequence; return true;
}

bool SensorHistory::readValuesJson(const std::string& deviceId, unsigned long before,
                                   std::string* json, std::string* error, ReadStats* stats) const {
    if (!plainText(deviceId, 64, false)) { if (error) *error = "invalid device ID"; return false; }
    if (!const_cast<SensorHistory*>(this)->initializeValues(error)) return false;
    std::vector<ReadPage> pages;
    pthread_mutex_lock(&values_.mutex);
    for (unsigned char metric = 1; metric <= maximumMetric; ++metric) {
        std::map<std::string, std::vector<unsigned int> >::const_iterator pair =
            values_.pairPages.find(pairKey(deviceId, metric));
        if (pair == values_.pairPages.end()) continue;
        for (unsigned long i = 0; i < pair->second.size(); ++i) {
            const unsigned int index = pair->second[i];
            const Page& page = values_.pages[index];
            if (before == 0 || page.oldest < before) pages.push_back(ReadPage(index, page));
        }
    }
    pthread_mutex_unlock(&values_.mutex);

#ifdef MICROHAD_TESTING
    if (sensorHistorySnapshotHook) sensorHistorySnapshotHook(sensorHistorySnapshotHookData);
#endif
    // All disk reads and response construction use only the copied catalog.
    std::sort(pages.begin(), pages.end(), newestPageFirst);
    int fd = open(values_.path.c_str(), O_RDONLY);
    if (fd < 0) { if (error) *error = "cannot read paged history"; return false; }
    unsigned char header[fileHeaderBytes];
    if (pread(fd, header, sizeof(header), 0) != fileHeaderBytes || !fileHeaderValid(header)) {
        close(fd); if (error) *error = "invalid paged history header"; return false;
    }
    if (stats) stats->bytesRead += fileHeaderBytes;
    std::vector<ValueRecord> selected;
    for (unsigned long p = 0; p < pages.size(); ++p) {
        const ReadPage& snapshot = pages[p];
        if (selected.size() == apiPageLimit + 1 && snapshot.page.newest <= selected.front().sequence) break;
        unsigned char pageData[pageBytes];
        off_t base = fileHeaderBytes + static_cast<off_t>(snapshot.index) * pageBytes;
        if (stats) { ++stats->pagesRead; stats->bytesRead += pageBytes; }
        if (pread(fd, pageData, sizeof(pageData), base) != static_cast<ssize_t>(sizeof(pageData))) continue;
        std::string owner; unsigned char metric = 0; uint32_t generation = 0;
        if (!pageHeaderValid(pageData, &owner, &metric, &generation) || owner != deviceId ||
            metric != snapshot.page.metric || generation != snapshot.page.generation) continue;
        unsigned char confirmed[pageHeaderBytes];
        if (stats) stats->bytesRead += pageHeaderBytes;
        if (pread(fd, confirmed, sizeof(confirmed), base) != static_cast<ssize_t>(sizeof(confirmed)) ||
            memcmp(confirmed, pageData, sizeof(confirmed)) != 0) continue;
        for (unsigned int i = 0; i < snapshot.page.recordCount; ++i) {
            const unsigned char* rec = pageData + pageHeaderBytes + i * recordBytes;
            const uint32_t seq = get32(rec);
            if (seq < snapshot.page.oldest || seq > snapshot.page.newest || (before != 0 && seq >= before)) continue;
            if (stats) ++stats->recordsChecked;
            if (rec[12] != recordCrc(pageData, rec)) continue;
            const uint32_t timestamp = get32(rec + 4);
            const float value = decodeFloat(rec + 8);
            if (timestamp == 0 || !std::isfinite(value) || (metric == AirQuality && qualityText(value).empty())) continue;
            const ValueRecord sample(seq, timestamp, metric, value);
            if (selected.size() < apiPageLimit + 1) {
                selected.push_back(sample);
                std::push_heap(selected.begin(), selected.end(), newerRecord);
            } else if (seq > selected.front().sequence) {
                std::pop_heap(selected.begin(), selected.end(), newerRecord);
                selected.back() = sample;
                std::push_heap(selected.begin(), selected.end(), newerRecord);
            }
        }
    }
    close(fd);
    const bool more = selected.size() > apiPageLimit;
    if (more) {
        std::pop_heap(selected.begin(), selected.end(), newerRecord);
        selected.pop_back();
    }
    std::sort(selected.begin(), selected.end(), olderRecord);
    std::ostringstream out; out << "{\"samples\":[";
    for (unsigned long i = 0; i < selected.size(); ++i) {
        const ValueRecord& sample = selected[i];
        if (i != 0) out << ',';
        out << '[' << sample.sequence << ',' << sample.timestamp << ',' << static_cast<unsigned int>(sample.metric) << ',';
        if (sample.metric == AirQuality) out << jsonString(qualityText(sample.value)); else out << numberText(sample.value);
        out << ',' << jsonString(unitForMetric(sample.metric)) << ']';
    }
    out << "],\"next_before\":" << (more && !selected.empty() ? selected.front().sequence : 0) << '}';
    *json = out.str(); return true;
}

bool SensorHistory::readForDevice(const std::string& address, unsigned long before,
                                  std::string* json, std::string* error, ReadStats* stats) const {
    const std::string::size_type separator = address.find(':');
    if (separator == std::string::npos) {
        if (address.size() != 18 || address[0] != '0' || address[1] != 'x') { if (error) *error = "invalid device address"; return false; }
        for (unsigned long i = 2; i < address.size(); ++i) {
            char c = address[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { if (error) *error = "invalid device address"; return false; }
        }
    }
    return readValuesJson(separator == std::string::npos ? "zigbee:" + address : address, before, json, error, stats);
}
bool SensorHistory::readJson(const std::string& path, const std::string& address, unsigned long before,
                             std::string* json, std::string* error, ReadStats* stats) {
    SensorHistory reader(path); if (!reader.initialize(error)) return false; return reader.readForDevice(address, before, json, error, stats);
}
