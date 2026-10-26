#include "sensor_history.h"
#include "storage.h"
#include "gateway.h"
#include "automation_profile.h"
#include "thread_name.h"

#include <algorithm>
#include <cerrno>
#include <set>
#include <poll.h>
#include <time.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MICROHAD_TESTING
void (*deviceCacheWriteHook)(void*) = 0;
void* deviceCacheWriteHookData = 0;
void (*sensorHistorySnapshotHook)(void*) = 0;
void* sensorHistorySnapshotHookData = 0;
void (*sensorHistoryPersistenceCopyHook)(void*) = 0;
void* sensorHistoryPersistenceCopyHookData = 0;
bool sensorHistoryFailPersistenceSync = false;
void (*sensorHistoryQueryPageHook)(void*) = 0;
void* sensorHistoryQueryPageHookData = 0;
#endif

namespace {
enum { fileHeaderBytes = 16, pageHeaderBytes = 86, recordBytes = 9, recordsPerPage = 218,
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
// Timestamp plus physical position is an exact JavaScript integer (at most 50 bits).
uint64_t recordCursor(uint32_t timestamp, unsigned int page, unsigned int slot) {
    return (static_cast<uint64_t>(timestamp) << 18) | (static_cast<uint64_t>(page) << 8) | slot;
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
// Split large cursors into decimal chunks: C++03 printf has no portable 64-bit
// modifier, and repeated 64-bit division is expensive on the gateway.
void appendUnsigned(std::string* out, uint64_t value) {
    char text[16];
    if (value < 1000000000UL) {
        snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(value));
    } else {
        const uint64_t high = value / 1000000000UL;
        appendUnsigned(out, high);
        snprintf(text, sizeof(text), "%09lu", static_cast<unsigned long>(value - high * 1000000000UL));
    }
    *out += text;
}
bool olderRecord(const SensorHistory::ValueRecord& a, const SensorHistory::ValueRecord& b) {
    return a.cursor < b.cursor;
}
bool newerRecord(const SensorHistory::ValueRecord& a, const SensorHistory::ValueRecord& b) {
    return a.cursor > b.cursor;
}
void selectRecord(std::vector<SensorHistory::ValueRecord>* selected, const SensorHistory::ValueRecord& sample) {
    if (selected->size() < apiPageLimit + 1) {
        selected->push_back(sample);
        std::push_heap(selected->begin(), selected->end(), newerRecord);
    } else if (sample.cursor > selected->front().cursor) {
        std::pop_heap(selected->begin(), selected->end(), newerRecord);
        selected->back() = sample;
        std::push_heap(selected->begin(), selected->end(), newerRecord);
    }
}
struct PageOrder {
    const std::vector<SensorHistory::Page>* pages;
    explicit PageOrder(const std::vector<SensorHistory::Page>* p) : pages(p) {}
    bool operator()(unsigned int a, unsigned int b) const { return (*pages)[a].generation < (*pages)[b].generation; }
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
bool normalizeNumeric(unsigned char inputMetric,double v,const std::string& unit,unsigned char* metric,float* number) {
    if(!std::isfinite(v)||!plainText(unit,16,true))return false;
    if(inputMetric==SensorHistory::Contact||inputMetric==SensorHistory::Occupancy||inputMetric==SensorHistory::Light){
        if(!unit.empty()||(v!=0&&v!=1))return false;
        *metric=inputMetric;*number=static_cast<float>(v);return true;
    }
    if(inputMetric==SensorHistory::AirQuality)return false;
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
    *metric = m; *number = static_cast<float>(v); return true;
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
    text->clear();return normalizeNumeric(inputMetric,v,unit,metric,number);
}

unsigned char recordCrc(const unsigned char* pageHeader, const unsigned char* record) {
    unsigned char data[71 + 4 + 8];
    memcpy(data, pageHeader, 71); memcpy(data + 71, pageHeader + 3, 4); memcpy(data + 75, record, 8);
    return crc8(data, sizeof(data));
}
void encodeFloat(unsigned char* b, float value) {
    uint32_t bits = 0; memcpy(&bits, &value, sizeof(bits)); put32(b, bits);
}
float decodeFloat(const unsigned char* b) {
    uint32_t bits = get32(b); float value = 0; memcpy(&value, &bits, sizeof(value)); return value;
}
std::string numberText(float value) {
    char text[32];
    snprintf(text, sizeof(text), "%.9g", static_cast<double>(value));
    return text;
}
std::string qualityText(float value) {
    static const char* const qualities[] = {"good", "fair", "moderate", "poor", "very_poor", "extremely_poor"};
    int index = static_cast<int>(value); return index >= 0 && index < 6 ? qualities[index] : "";
}
bool pageHeaderValid(const unsigned char* h, std::string* id, unsigned char* metric, uint32_t* generation) {
    if (h[2] != 1 || h[0] == 0 || h[0] > 64 || h[1] < 1 || h[1] > maximumMetric || h[85] != crc8(h, 85)) return false;
    for (int i = 71; i <= 84; ++i) if (h[i] != 0) return false;
    id->assign(reinterpret_cast<const char*>(h + 7), h[0]);
    *metric = h[1]; *generation = get32(h + 3);
    return plainText(*id, 64, false) && *generation != 0;
}
void makePageHeader(unsigned char* h, const std::string& id, unsigned char metric, uint32_t generation) {
    memset(h, 0, pageHeaderBytes); h[0] = static_cast<unsigned char>(id.size()); h[1] = metric; h[2] = 1;
    put32(h + 3, generation); memcpy(h + 7, id.data(), id.size()); h[85] = crc8(h, 85);
}
}


namespace {
enum { flashPageLimit = 255, flashFileBytes = fileHeaderBytes + flashPageLimit * pageBytes,
       sparseInterval = 4 * 60 * 60 };
unsigned long monotonicSeconds() {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ? static_cast<unsigned long>(now.tv_sec) : 0;
}
bool samePage(const SensorHistory::Page& a, const SensorHistory::Page& b) {
    return a.allocated && b.allocated && a.generation == b.generation && a.id == b.id && a.metric == b.metric;
}
bool historyPageAllowed(const SensorHistory::Page& p) {
    return p.metric != SensorHistory::Light && p.metric != SensorHistory::Brightness && p.id.find("fritz:") != 0;
}
// Decode contiguous records only: CRC8 remains the explicitly chosen recovery protection.
bool decodePage(const unsigned char* bytes, SensorHistory::Page* page) {
    uint32_t generation = 0;
    if (!pageHeaderValid(bytes, &page->id, &page->metric, &generation)) return false;
    page->generation = generation; page->allocated = true; page->recordCount = 0;
    if (!historyPageAllowed(*page)) return false;
    for (unsigned int r = 0; r < recordsPerPage; ++r) {
        const unsigned char* record = bytes + pageHeaderBytes + r * recordBytes;
        const uint32_t timestamp = get32(record);
        const float value = decodeFloat(record + 4);
        if (record[8] != recordCrc(bytes, record) || timestamp == 0 || !std::isfinite(value) ||
            (page->metric == SensorHistory::AirQuality && qualityText(value).empty())) break;
        ++page->recordCount;
    }
    return page->recordCount != 0;
}
struct FlashCandidate {
    SensorHistory::Page page;
    int ramIndex;
    int slot;
    FlashCandidate(const SensorHistory::Page& p, int r, int s) : page(p), ramIndex(r), slot(s) {}
};
bool newestAllocation(const FlashCandidate& a, const FlashCandidate& b) { return a.page.generation > b.page.generation; }
}

struct SensorHistory::FlashState {
    std::string path;
    std::string error;
    bool prepared, disabled, started, stopping, manual, retrySparse;
    pthread_t thread;
    int wakeRead, wakeWrite;
    unsigned long nextSparse, retryAt, pageWrites;
    std::vector<Page> residents;
    std::vector<Device> cachePending;
    std::string cachePath, cacheLegacyPath, cacheError;
    bool cacheQueued, cacheWriting, cacheForce;
    uint64_t cacheSubmitted, cachePendingGeneration, cacheCompleted, cacheFailed, cacheFailures;
    unsigned long cacheRetryAt;
    pthread_cond_t cacheReady;
    explicit FlashState(const std::string& p) : path(p), prepared(false), disabled(false), started(false),
        stopping(false), manual(false), retrySparse(false), wakeRead(-1), wakeWrite(-1), nextSparse(sparseInterval), retryAt(0), pageWrites(0),
        residents(flashPageLimit), cacheQueued(false), cacheWriting(false), cacheForce(false),
        cacheSubmitted(0), cachePendingGeneration(0), cacheCompleted(0), cacheFailed(0), cacheFailures(0), cacheRetryAt(0) {
        pthread_cond_init(&cacheReady, 0);
        {
            int descriptors[2];
            if (pipe(descriptors) == 0) {
                wakeRead = descriptors[0]; wakeWrite = descriptors[1];
                fcntl(wakeRead, F_SETFL, fcntl(wakeRead, F_GETFL) | O_NONBLOCK);
                fcntl(wakeWrite, F_SETFL, fcntl(wakeWrite, F_GETFL) | O_NONBLOCK);
            } else { disabled = true; error = "cannot create history persistence wakeup pipe"; }
        }
    }
    void signal() {
        if (wakeWrite >= 0) { const char byte = 1; ssize_t ignored = write(wakeWrite, &byte, 1); (void)ignored; }
    }
    ~FlashState() { pthread_cond_destroy(&cacheReady); if (wakeRead >= 0) close(wakeRead); if (wakeWrite >= 0) close(wakeWrite); }
};

bool SensorHistory::prepareFlash(std::string* error) {
    if (flash_->path.empty() || flash_->disabled) return true;
    if (!flash_->prepared) {
        flash_->prepared = true;
        int fd = open(flash_->path.c_str(), O_RDONLY);
        if (fd < 0 && errno != ENOENT) {
            flash_->disabled = true; flash_->error = "cannot read flash history; persistence disabled";
        } else if (fd >= 0) {
            struct stat st; unsigned char header[fileHeaderBytes];
            bool valid = fstat(fd, &st) == 0 && st.st_size >= fileHeaderBytes && st.st_size <= flashFileBytes &&
                pread(fd, header, sizeof(header), 0) == fileHeaderBytes && fileHeaderValid(header);
            if (!valid) {
                flash_->disabled = true; flash_->error = "invalid flash history header; file preserved, persistence disabled";
            } else {
                for (unsigned int i = 0; i < flashPageLimit; ++i) {
                    const off_t at = fileHeaderBytes + static_cast<off_t>(i) * pageBytes;
                    if (at + pageBytes > st.st_size) break;
                    unsigned char bytes[pageBytes]; Page page;
                    if (pread(fd, bytes, sizeof(bytes), at) == pageBytes && decodePage(bytes, &page))
                        flash_->residents[i] = page;
                }
            }
            close(fd);
        }
    }
    struct stat ram;
    if (stat(values_.path.c_str(), &ram) == 0) return true;
    if (errno != ENOENT) { if (error) *error = "cannot inspect RAM history"; return false; }
    if (flash_->disabled) return true;
    int source = open(flash_->path.c_str(), O_RDONLY);
    if (source < 0) return true; // Missing flash starts fresh in RAM.
    std::string temporary = values_.path + ".restore-XXXXXX";
    std::vector<char> name(temporary.begin(), temporary.end()); name.push_back(0);
    int target = mkstemp(&name[0]);
    if (target < 0) { close(source); if (error) *error = "cannot restore flash history into RAM"; return false; }
    unsigned char header[fileHeaderBytes]; memcpy(header, fileTemplate, sizeof(header)); header[15] = crc8(header, 15);
    bool ok = writeAllAt(target, header, sizeof(header), 0);
    unsigned int restored = 0;
    for (unsigned int i = 0; ok && i < flashPageLimit; ++i) {
        if (!flash_->residents[i].allocated) continue;
        unsigned char bytes[pageBytes]; Page page;
        const off_t at = fileHeaderBytes + static_cast<off_t>(i) * pageBytes;
        if (pread(source, bytes, sizeof(bytes), at) != pageBytes || !decodePage(bytes, &page) ||
            !samePage(page, flash_->residents[i])) continue;
        ok = writeAllAt(target, bytes, sizeof(bytes), fileHeaderBytes + static_cast<off_t>(restored) * pageBytes);
        if (ok) ++restored;
    }
    close(source); close(target);
    if (ok) ok = rename(&name[0], values_.path.c_str()) == 0;
    if (!ok) { unlink(&name[0]); if (error) *error = "cannot restore flash history into RAM"; }
    return ok;
}

std::string SensorHistory::takePersistenceError() {
    pthread_mutex_lock(&values_.mutex); std::string result = flash_->error; flash_->error.clear();
    pthread_mutex_unlock(&values_.mutex); return result;
}

bool SensorHistory::persistPages(unsigned long seconds, std::string* error) {
    if (error) error->clear();
    if (flash_->path.empty() || flash_->disabled || seconds < flash_->retryAt) return true;
    const bool sparse = flash_->retrySparse || seconds >= flash_->nextSparse;
    if (seconds >= flash_->nextSparse)
        flash_->nextSparse += ((seconds - flash_->nextSparse) / sparseInterval + 1) * sparseInterval;
    std::vector<Page> ram;
    pthread_mutex_lock(&values_.mutex); ram = values_.pages; bool stopping = flash_->stopping;
    pthread_mutex_unlock(&values_.mutex);
    if (stopping) return true;
    std::vector<FlashCandidate> candidates;
    for (unsigned int i = 0; i < flashPageLimit; ++i)
        if (flash_->residents[i].allocated) candidates.push_back(FlashCandidate(flash_->residents[i], -1, i));
    for (unsigned int i = 0; i < ram.size(); ++i) {
        if (!ram[i].allocated || !historyPageAllowed(ram[i]) || (ram[i].recordCount < recordsPerPage && !sparse)) continue;
        bool found = false;
        for (unsigned int j = 0; j < candidates.size(); ++j) {
            if (!samePage(candidates[j].page, ram[i])) continue;
            found = true;
            if (ram[i].recordCount > candidates[j].page.recordCount) {
                candidates[j].page = ram[i]; candidates[j].ramIndex = i;
            }
            break;
        }
        if (!found) candidates.push_back(FlashCandidate(ram[i], i, -1));
    }
    std::sort(candidates.begin(), candidates.end(), newestAllocation);
    std::vector<FlashCandidate> selected; std::set<std::string> pairs; std::set<unsigned long> generations;
    for (unsigned int i = 0; i < candidates.size() && selected.size() < flashPageLimit; ++i) {
        if (!pairs.insert(pairKey(candidates[i].page.id, candidates[i].page.metric)).second) continue;
        selected.push_back(candidates[i]); generations.insert(candidates[i].page.generation);
    }
    for (unsigned int i = 0; i < candidates.size() && selected.size() < flashPageLimit; ++i)
        if (generations.insert(candidates[i].page.generation).second) selected.push_back(candidates[i]);
    bool used[flashPageLimit]; memset(used, 0, sizeof(used));
    bool pending = false;
    for (unsigned int i = 0; i < selected.size(); ++i) {
        if (selected[i].slot >= 0) used[selected[i].slot] = true;
        if (selected[i].ramIndex >= 0) pending = true;
    }
    if (!pending) { flash_->retrySparse = false; return true; }
    int source = open(values_.path.c_str(), O_RDONLY);
    int target = open(flash_->path.c_str(), O_RDWR | O_CREAT, 0600);
    bool ok = source >= 0 && target >= 0;
    if (ok) {
        struct stat st; unsigned char header[fileHeaderBytes];
        ok = fstat(target, &st) == 0 && st.st_size <= flashFileBytes;
        if (ok && st.st_size == 0) {
            memcpy(header, fileTemplate, sizeof(header)); header[15] = crc8(header, 15);
            ok = writeAllAt(target, header, sizeof(header), 0);
        } else if (ok) ok = pread(target, header, sizeof(header), 0) == fileHeaderBytes && fileHeaderValid(header);
    }
    std::vector<FlashCandidate> written;
    for (unsigned int i = 0; ok && i < selected.size(); ++i) {
        FlashCandidate candidate = selected[i];
        if (candidate.ramIndex < 0) continue;
        pthread_mutex_lock(&values_.mutex); stopping = flash_->stopping; pthread_mutex_unlock(&values_.mutex);
        if (stopping) break;
        unsigned char bytes[pageBytes], confirmed[pageHeaderBytes]; Page actual;
        off_t at = fileHeaderBytes + static_cast<off_t>(candidate.ramIndex) * pageBytes;
        if (pread(source, bytes, sizeof(bytes), at) != pageBytes || !decodePage(bytes, &actual) ||
            !samePage(actual, candidate.page) || actual.recordCount < candidate.page.recordCount ||
            pread(source, confirmed, sizeof(confirmed), at) != pageHeaderBytes ||
            memcmp(confirmed, bytes, sizeof(confirmed)) != 0) continue;
        if (candidate.slot < 0) {
            // Surviving residents already own their slots. Never move them to compact the file.
            for (unsigned int slot = 0; slot < flashPageLimit; ++slot)
                if (!used[slot] && !flash_->residents[slot].allocated) { candidate.slot = slot; break; }
            if (candidate.slot < 0)
                for (unsigned int slot = 0; slot < flashPageLimit; ++slot)
                    if (!used[slot]) { candidate.slot = slot; break; }
        }
        if (candidate.slot < 0) continue;
        used[candidate.slot] = true;
#ifdef MICROHAD_TESTING
        if (sensorHistoryPersistenceCopyHook) sensorHistoryPersistenceCopyHook(sensorHistoryPersistenceCopyHookData);
#endif
        // Reserve ownership before writing, even if a short write or fsync fails.
        // Retry must use this same slot; record count is acknowledged only after sync.
        Page reserved = actual;
        reserved.recordCount = samePage(flash_->residents[candidate.slot], actual) ?
            flash_->residents[candidate.slot].recordCount : 0;
        flash_->residents[candidate.slot] = reserved;
        at = fileHeaderBytes + static_cast<off_t>(candidate.slot) * pageBytes;
        ok = writeAllAt(target, bytes, sizeof(bytes), at);
        if (ok) { candidate.page = actual; written.push_back(candidate); }
    }
    bool synced = target >= 0 && fsync(target) == 0;
#ifdef MICROHAD_TESTING
    if (sensorHistoryFailPersistenceSync) synced = false;
#endif
    if (synced) {
        for (unsigned int i = 0; i < written.size(); ++i) flash_->residents[written[i].slot] = written[i].page;
        pthread_mutex_lock(&values_.mutex); flash_->pageWrites += written.size(); pthread_mutex_unlock(&values_.mutex);
    }
    if (source >= 0) close(source);
    if (target >= 0) close(target);
    ok = ok && synced;
    flash_->retrySparse = !ok && sparse;
    flash_->retryAt = ok ? 0 : seconds + 60;
    if (!ok && error) *error = "flash history write failed; pending pages will be retried";
    return ok;
}

void* SensorHistory::persistenceThread(void* opaque) {
    setCurrentThreadName("history-write");
    static_cast<SensorHistory*>(opaque)->runPersistence(); return 0;
}
bool SensorHistory::startPersistenceLocked(std::string* error) {
    if (flash_->started || flash_->manual) return true;
    if (flash_->stopping || flash_->wakeRead < 0 || flash_->wakeWrite < 0 ||
        pthread_create(&flash_->thread, 0, persistenceThread, this) != 0) {
        if (error) *error="cannot start persistence worker";
        return false;
    }
    flash_->started=true;return true;
}
bool SensorHistory::queueDeviceCache(const std::string& path,std::vector<Device>* snapshot,
                                    const std::string& legacyPath,std::string* error) {
    if (error) error->clear();
    pthread_mutex_lock(&values_.mutex);
    if (!startPersistenceLocked(error)) { pthread_mutex_unlock(&values_.mutex);return false; }
    flash_->cachePath=path;flash_->cacheLegacyPath=legacyPath;
    flash_->cachePending.swap(*snapshot);flash_->cacheQueued=true;
    flash_->cachePendingGeneration=++flash_->cacheSubmitted;
    flash_->signal();pthread_mutex_unlock(&values_.mutex);return true;
}
bool SensorHistory::persistDeviceCache(unsigned long seconds,bool force,std::string* error) {
    std::vector<Device> snapshot;std::string path,legacy;
    pthread_mutex_lock(&values_.mutex);
    if (!flash_->cacheQueued || (!force && !flash_->cacheForce && seconds<flash_->cacheRetryAt)) {
        pthread_mutex_unlock(&values_.mutex);return true;
    }
    snapshot.swap(flash_->cachePending);path=flash_->cachePath;legacy=flash_->cacheLegacyPath;
    const uint64_t generation=flash_->cachePendingGeneration;
    flash_->cacheQueued=false;flash_->cacheWriting=true;flash_->cacheForce=false;
    pthread_mutex_unlock(&values_.mutex);
    std::string failure;bool ok;
    {
        AUTOMATION_PROFILE("history.cacheWrite");
#ifdef MICROHAD_TESTING
        extern void (*deviceCacheWriteHook)(void*);
        extern void* deviceCacheWriteHookData;
        if (deviceCacheWriteHook) deviceCacheWriteHook(deviceCacheWriteHookData);
#endif
        ok=saveDeviceCache(path,snapshot,&failure);
    }
    if (ok && !legacy.empty() && legacy!=path) unlink(legacy.c_str());
    pthread_mutex_lock(&values_.mutex);
    flash_->cacheWriting=false;
    if (ok) {
        flash_->cacheCompleted=generation;flash_->cacheRetryAt=0;flash_->cacheError.clear();
    } else {
        flash_->cacheFailed=generation;++flash_->cacheFailures;flash_->cacheRetryAt=seconds+5;flash_->cacheError=failure;
        flash_->error=failure;
        if (!flash_->cacheQueued) {
            snapshot.swap(flash_->cachePending);flash_->cacheQueued=true;flash_->cachePendingGeneration=generation;
        }
        if (error) *error=failure;
    }
    pthread_cond_broadcast(&flash_->cacheReady);pthread_mutex_unlock(&values_.mutex);
    return ok;
}
bool SensorHistory::flushDeviceCache(std::string* error) {
    if (error) error->clear();
    pthread_mutex_lock(&values_.mutex);
    const uint64_t target=flash_->cacheSubmitted;
    if (flash_->cacheCompleted>=target) { pthread_mutex_unlock(&values_.mutex);return true; }
    flash_->cacheForce=true;flash_->signal();
    // A failed attempt wakes this barrier; shutdown must not retry forever.
    const uint64_t failedBefore=flash_->cacheFailures;
    if (flash_->manual) {
        pthread_mutex_unlock(&values_.mutex);
        return persistDeviceCache(monotonicSeconds(),true,error);
    }
    while (flash_->cacheCompleted<target && (flash_->cacheFailed<target || flash_->cacheFailures==failedBefore))
        pthread_cond_wait(&flash_->cacheReady,&values_.mutex);
    const bool ok=flash_->cacheCompleted>=target;
    if (!ok && error) *error=flash_->cacheError;
    pthread_mutex_unlock(&values_.mutex);return ok;
}

void SensorHistory::runPersistence() {
    for (;;) {
        std::string error;
        persistDeviceCache(monotonicSeconds(), false, &error);
        // Cache jobs also work when history initialization failed or is disabled.
        pthread_mutex_lock(&values_.mutex);const bool historyReady=values_.initialized;pthread_mutex_unlock(&values_.mutex);
        if (historyReady) persistPages(monotonicSeconds(), &error);
        pthread_mutex_lock(&values_.mutex);
        if (!error.empty()) flash_->error = error;
        if (flash_->stopping) { pthread_mutex_unlock(&values_.mutex); break; }
        pthread_mutex_unlock(&values_.mutex);
        unsigned long now = monotonicSeconds();
        pthread_mutex_lock(&values_.mutex);
        unsigned long delay = historyReady && !flash_->disabled && flash_->nextSparse > now ? flash_->nextSparse - now : 60;
        if (flash_->cacheQueued) delay = std::min(delay, flash_->cacheForce || flash_->cacheRetryAt <= now ? 0UL : flash_->cacheRetryAt - now);
        pthread_mutex_unlock(&values_.mutex);
        struct pollfd descriptor; descriptor.fd = flash_->wakeRead; descriptor.events = POLLIN; descriptor.revents = 0;
        // Relative poll timeout avoids wall-clock changes affecting the monotonic schedule.
        poll(&descriptor, 1, static_cast<int>(std::min(delay, 60UL) * 1000));
        char bytes[64];
        while (read(flash_->wakeRead, bytes, sizeof(bytes)) > 0) {}

    }
}
#ifdef MICROHAD_TESTING
void SensorHistory::useManualPersistenceForTests() { flash_->manual = true; }
bool SensorHistory::persistForTests(unsigned long seconds, std::string* error) {
    return initializeValues(error) && persistPages(seconds, error);
}
unsigned long SensorHistory::persistenceWritesForTests() const {
    pthread_mutex_lock(&values_.mutex); unsigned long result = flash_->pageWrites;
    pthread_mutex_unlock(&values_.mutex); return result;
}
#endif

SensorHistory::ValueJournal::ValueJournal() : generation(0), initialized(false) { pthread_mutex_init(&mutex, 0); }
SensorHistory::ValueJournal::~ValueJournal() { pthread_mutex_destroy(&mutex); }
SensorHistory::SensorHistory(const std::string& path, const std::string& flashPath)
    : flash_(new FlashState(flashPath)) { values_.path = path + ".values"; }
SensorHistory::~SensorHistory() {
    flushDeviceCache();
    pthread_mutex_lock(&values_.mutex);
    flash_->stopping = true; flash_->signal();
    pthread_mutex_unlock(&values_.mutex);
    if (flash_->started) pthread_join(flash_->thread, 0);
    delete flash_;
}
const std::string& SensorHistory::path() const { return values_.path; }

bool SensorHistory::initialize(std::string* error) {
    if (error) error->clear();
    return initializeValues(error);
}
bool SensorHistory::initializeValues(std::string* error) {
    pthread_mutex_lock(&values_.mutex);
    if (values_.initialized) { pthread_mutex_unlock(&values_.mutex); return true; }
    if (!prepareFlash(error)) { pthread_mutex_unlock(&values_.mutex); return false; }
    int fd = open(values_.path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) { if (error) *error = "cannot open paged history"; pthread_mutex_unlock(&values_.mutex); return false; }
    struct stat st; bool ok = fstat(fd, &st) == 0;
    unsigned char h[fileHeaderBytes];
    if (ok && st.st_size == 0) {
        memcpy(h, fileTemplate, sizeof(h)); h[15] = crc8(h, 15);
        ok = writeAllAt(fd, h, sizeof(h), 0); st.st_size = fileHeaderBytes;
    }
    if (ok) ok = pread(fd, h, sizeof(h), 0) == fileHeaderBytes && fileHeaderValid(h);
    if (ok && st.st_size > maximumFileBytes) ok = false;
    if (!ok) { if (error) *error = "invalid paged history header; file preserved"; close(fd); pthread_mutex_unlock(&values_.mutex); return false; }

    values_.pages.assign(pageLimit, Page()); values_.pairPages.clear(); values_.lastValues.clear(); values_.generation = 0;
    for (unsigned int i = 0; i < flash_->residents.size(); ++i)
        if (flash_->residents[i].generation > values_.generation) values_.generation = flash_->residents[i].generation;
    for (unsigned int i = 0; i < pageLimit; ++i) {
        off_t base = fileHeaderBytes + static_cast<off_t>(i) * pageBytes;
        if (base + pageHeaderBytes > st.st_size) break;
        unsigned char ph[pageHeaderBytes];
        if (pread(fd, ph, sizeof(ph), base) != static_cast<ssize_t>(sizeof(ph))) continue;
        std::string id; unsigned char metric = 0; uint32_t generation = 0;
        if (!pageHeaderValid(ph, &id, &metric, &generation)) continue;
        Page& page = values_.pages[i]; page.allocated = true; page.id = id; page.metric = metric; page.generation = generation;
        if (generation > values_.generation) values_.generation = generation;
        for (unsigned int r = 0; r < recordsPerPage; ++r) {
            unsigned char rec[recordBytes]; off_t at = base + pageHeaderBytes + static_cast<off_t>(r) * recordBytes;
            if (at + recordBytes > st.st_size || pread(fd, rec, sizeof(rec), at) != static_cast<ssize_t>(sizeof(rec))) break;
            if (rec[8] != recordCrc(ph, rec)) break;
            uint32_t timestamp = get32(rec); float v = decodeFloat(rec + 4);
            const uint64_t cursor = recordCursor(timestamp, i, r);
            if (timestamp == 0 || !std::isfinite(v)) break;
            std::string text = metric == AirQuality ? qualityText(v) : numberText(v);
            if (text.empty()) break;
            ++page.recordCount;
            if (page.oldest == 0 || cursor < page.oldest) page.oldest = cursor;
            if (cursor > page.newest) page.newest = cursor;
            page.lastValue = v; page.lastTimestamp = timestamp;
            const std::string key = pairKey(id, metric);
            std::map<std::string, LastValue>::iterator previous = values_.lastValues.find(key);
            if (previous == values_.lastValues.end() || generation >= previous->second.generation)
                values_.lastValues[key] = LastValue(generation, timestamp, v);
        }
        if (page.recordCount != 0) {
            const std::string key = pairKey(id, metric); values_.pairPages[key].push_back(i);
        } else page.allocated = false;
    }
    for (std::map<std::string, std::vector<unsigned int> >::iterator pair = values_.pairPages.begin();
         pair != values_.pairPages.end(); ++pair)
        std::sort(pair->second.begin(), pair->second.end(), PageOrder(&values_.pages));
    close(fd); values_.initialized = true;
    if (!flash_->path.empty() && !flash_->disabled && !flash_->manual) {
        flash_->nextSparse = monotonicSeconds() + 4 * 60 * 60;
        std::string workerError;
        if (!startPersistenceLocked(&workerError)) { flash_->disabled = true; flash_->error = workerError; }
    }
    pthread_mutex_unlock(&values_.mutex); return true;
}

bool SensorHistory::appendValue(const std::string& deviceId, unsigned char inputMetric, const std::string& value,
                               const std::string& unit, long timestamp) {
    unsigned char metric=0;float numeric=0;std::string text;
    if(!normalizeValue(inputMetric,value,unit,&metric,&numeric,&text)||!initializeValues(0))return false;
    pthread_mutex_lock(&values_.mutex);
    bool result=appendNormalizedLocked(deviceId,metric,numeric,timestamp);
    pthread_mutex_unlock(&values_.mutex);return result;
}
bool SensorHistory::appendNumeric(const std::string& deviceId,unsigned char inputMetric,double value,
                                 const std::string& unit,long timestamp) {
    unsigned char metric=0;float numeric=0;
    if(!normalizeNumeric(inputMetric,value,unit,&metric,&numeric)||!initializeValues(0))return false;
    pthread_mutex_lock(&values_.mutex);
    bool result=appendNormalizedLocked(deviceId,metric,numeric,timestamp);
    pthread_mutex_unlock(&values_.mutex);return result;
}
bool SensorHistory::appendNormalizedLocked(const std::string& deviceId,unsigned char metric,float numeric,long timestamp) {
    // Actuator states and FRITZ!Box presence are excluded for both entry points.
    if(metric==Light||metric==Brightness||deviceId.find("fritz:")==0)return false;
    if(!values_.initialized||!plainText(deviceId,64,false)||timestamp<=0||static_cast<unsigned long>(timestamp)>0xffffffffUL)return false;
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
    unsigned long nextGeneration = values_.generation + 1;
    if (page == 0) {
        if (values_.generation >= 0xffffffffUL) { close(fd); return false; }
        bool found = false;
        for (unsigned int i = 0; i < values_.pages.size(); ++i) if (!values_.pages[i].allocated) { pageIndex = i; found = true; break; }
        if (!found) {
            unsigned long oldest = 0xffffffffUL; int victim = -1;
            for (unsigned int i = 0; i < values_.pages.size(); ++i) {
                Page& candidate = values_.pages[i];
                std::string candidateKey = pairKey(candidate.id, candidate.metric);
                std::vector<unsigned int>& siblings = values_.pairPages[candidateKey];
                if (candidateKey == key && siblings.size() <= 1) continue;
                if (victim < 0 || candidate.generation < oldest) { oldest = candidate.generation; victim = static_cast<int>(i); }
            }
            if (victim < 0) { close(fd); return false; }
            pageIndex = static_cast<unsigned int>(victim);
            Page& evicted = values_.pages[pageIndex];
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
                    if (remaining.generation > latest) {
                        latest = remaining.generation;
                        newest = LastValue(remaining.generation, remaining.lastTimestamp, remaining.lastValue);
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
        values_.generation = nextGeneration;
        owned.push_back(pageIndex); page = &allocated;
    } else pageIndex = static_cast<unsigned int>(page - &values_.pages[0]);

    unsigned char rec[recordBytes]; put32(rec, static_cast<uint32_t>(timestamp));
    encodeFloat(rec + 4, numeric);
    unsigned char ph[pageHeaderBytes]; off_t base = fileHeaderBytes + static_cast<off_t>(pageIndex) * pageBytes;
    if (pread(fd, ph, sizeof(ph), base) != static_cast<ssize_t>(sizeof(ph))) { close(fd); return false; }
    rec[8] = recordCrc(ph, rec);
    unsigned int slot = page->recordCount;
    if (slot >= recordsPerPage) { close(fd); return false; }
    off_t offset = base + pageHeaderBytes + static_cast<off_t>(slot) * recordBytes;
    bool ok = writeAllAt(fd, rec, sizeof(rec), offset); close(fd); if (!ok) return false;
    ++page->recordCount; page->lastValue = numeric;
    if (page->recordCount == recordsPerPage) flash_->signal();
    const uint64_t cursor = recordCursor(static_cast<uint32_t>(timestamp), pageIndex, slot);
    if (page->oldest == 0 || cursor < page->oldest) page->oldest = cursor;
    if (cursor > page->newest) page->newest = cursor;
    page->lastTimestamp = static_cast<uint32_t>(timestamp);
    values_.lastValues[key] = LastValue(page->generation, static_cast<unsigned long>(timestamp), numeric);
    return true;
}

bool SensorHistory::readValuesJson(const std::vector<std::string>& deviceIds, uint64_t before,
                                   std::string* json, std::string* error, ReadStats* stats, uint32_t since) const {
    if (deviceIds.empty()) { if (error) *error = "invalid device ID"; return false; }
    for (unsigned long idIndex = 0; idIndex < deviceIds.size(); ++idIndex)
        if (!plainText(deviceIds[idIndex], 64, false)) { if (error) *error = "invalid device ID"; return false; }
    if (!const_cast<SensorHistory*>(this)->initializeValues(error)) return false;
    std::vector<ReadPage> pages;
    pthread_mutex_lock(&values_.mutex);
    for (unsigned long idIndex = 0; idIndex < deviceIds.size(); ++idIndex) {
        for (unsigned char metric = 1; metric <= maximumMetric; ++metric) {
            std::map<std::string, std::vector<unsigned int> >::const_iterator pair =
                values_.pairPages.find(pairKey(deviceIds[idIndex], metric));
            if (pair == values_.pairPages.end()) continue;
            for (unsigned long i = 0; i < pair->second.size(); ++i) {
                const unsigned int index = pair->second[i];
                const Page& page = values_.pages[index];
                // Predecessors must be found independently of the pagination cursor,
                // otherwise each older page could return another predecessor.
                if (before == 0 || page.oldest < before || (since && (page.oldest >> 18) < since))
                    pages.push_back(ReadPage(index, page));
            }
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
    ValueRecord predecessors[maximumMetric + 1];
    for (unsigned long p = 0; p < pages.size(); ++p) {
        const ReadPage& snapshot = pages[p];
        if (selected.size() == apiPageLimit + 1 && snapshot.page.newest <= selected.front().cursor) break;
        if (since && (snapshot.page.newest >> 18) < since &&
            predecessors[snapshot.page.metric].cursor >= snapshot.page.newest) continue;
        unsigned char pageData[pageBytes];
        off_t base = fileHeaderBytes + static_cast<off_t>(snapshot.index) * pageBytes;
        if (stats) { ++stats->pagesRead; stats->bytesRead += pageBytes; }
        if (pread(fd, pageData, sizeof(pageData), base) != static_cast<ssize_t>(sizeof(pageData))) continue;
        std::string owner; unsigned char metric = 0; uint32_t generation = 0;
        if (!pageHeaderValid(pageData, &owner, &metric, &generation) ||
            std::find(deviceIds.begin(), deviceIds.end(), owner) == deviceIds.end() ||
            metric != snapshot.page.metric || generation != snapshot.page.generation) continue;
        unsigned char confirmed[pageHeaderBytes];
        if (stats) stats->bytesRead += pageHeaderBytes;
        if (pread(fd, confirmed, sizeof(confirmed), base) != static_cast<ssize_t>(sizeof(confirmed)) ||
            memcmp(confirmed, pageData, sizeof(confirmed)) != 0) continue;
        for (unsigned int i = 0; i < snapshot.page.recordCount; ++i) {
            const unsigned char* rec = pageData + pageHeaderBytes + i * recordBytes;
            const uint32_t timestamp = get32(rec);
            const uint64_t cursor = recordCursor(timestamp, snapshot.index, i);
            if (cursor < snapshot.page.oldest || cursor > snapshot.page.newest) continue;
            if ((since == 0 || timestamp >= since) && before != 0 && cursor >= before) continue;
            if (stats) ++stats->recordsChecked;
            if (rec[8] != recordCrc(pageData, rec)) continue;
            const float value = decodeFloat(rec + 4);
            if (timestamp == 0 || !std::isfinite(value) || (metric == AirQuality && qualityText(value).empty())) continue;
            const ValueRecord sample(cursor, timestamp, metric, value);
            if (since && timestamp < since) {
                if (cursor > predecessors[metric].cursor) predecessors[metric] = sample;
            } else selectRecord(&selected, sample);
        }
    }
    close(fd);
    if (since) {
        for (unsigned int metric = 1; metric <= maximumMetric; ++metric) {
            const ValueRecord& prior = predecessors[metric];
            if (prior.cursor && (before == 0 || prior.cursor < before)) selectRecord(&selected, prior);
        }
    }
    const bool more = selected.size() > apiPageLimit;
    if (more) {
        std::pop_heap(selected.begin(), selected.end(), newerRecord);
        selected.pop_back();
    }
    std::sort(selected.begin(), selected.end(), olderRecord);
    std::string out;
    out.reserve(selected.size() * 64 + 64);
    out = "{\"samples\":[";
    char text[96];
    for (unsigned long i = 0; i < selected.size(); ++i) {
        const ValueRecord& sample = selected[i];
        if (i != 0) out += ',';
        out += '[';
        appendUnsigned(&out, sample.cursor);
        snprintf(text, sizeof(text), ",%lu,%u,", sample.timestamp, static_cast<unsigned int>(sample.metric));
        out += text;
        if (sample.metric == AirQuality) {
            out += '"'; out += qualityText(sample.value); out += '"';
        } else {
            snprintf(text, sizeof(text), "%.9g", static_cast<double>(sample.value));
            out += text;
        }
        // Units come from the fixed metric table and contain no JSON escapes.
        out += ",\""; out += unitForMetric(sample.metric); out += "\"]";
    }
    out += "],\"next_before\":";
    appendUnsigned(&out, more && !selected.empty() ? selected.front().cursor : 0);
    out += '}';
    json->swap(out); return true;
}

bool SensorHistory::readForDevice(const std::string& address, uint64_t before,
                                  std::string* json, std::string* error, ReadStats* stats, uint32_t since) const {
    return readForDevices(std::vector<std::string>(1, address), before, json, error, stats, since);
}

bool SensorHistory::readForDevices(const std::vector<std::string>& deviceIds, uint64_t before,
                                   std::string* json, std::string* error, ReadStats* stats, uint32_t since) const {
    std::vector<std::string> normalized;
    for (unsigned long index = 0; index < deviceIds.size(); ++index) {
        const std::string& address = deviceIds[index];
        if (address.find(':') == std::string::npos) {
            if (address.size() != 18 || address[0] != '0' || address[1] != 'x') { if (error) *error = "invalid device address"; return false; }
            for (unsigned long i = 2; i < address.size(); ++i) {
                char c = address[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { if (error) *error = "invalid device address"; return false; }
            }
            normalized.push_back("zigbee:" + address);
        } else normalized.push_back(address);
    }
    return readValuesJson(normalized, before, json, error, stats, since);
}
bool SensorHistory::readJson(const std::string& path, const std::string& address, uint64_t before,
                             std::string* json, std::string* error, ReadStats* stats) {
    SensorHistory reader(path); if (!reader.initialize(error)) return false; return reader.readForDevice(address, before, json, error, stats);
}


struct SensorHistory::Query::State {
    std::vector<ReadPage> pages;
    size_t position;
    uint64_t before;
    uint32_t since;
    unsigned int limit;
    bool filterValue;
    float value;
    State() : position(0), before(0), since(0), limit(128), filterValue(false), value(0) {}
};
SensorHistory::Query::Query() : nextBefore(0), done(false), state(new State) {}
SensorHistory::Query::~Query() { delete state; }
std::string SensorHistory::metricUnit(unsigned char metric) { return unitForMetric(metric); }
std::string SensorHistory::valueText(unsigned char metric,float value) {
    return metric==AirQuality ? qualityText(value) : "";
}
SensorHistory::Query* SensorHistory::beginQuery(const std::vector<std::string>& ids,
        unsigned char metric,uint64_t before,uint32_t since,unsigned int limit,
        bool filterValue,float value,std::string* error) const {
    if(ids.empty()||metric<1||metric>maximumMetric||limit<1||limit>256) {
        if(error)*error="invalid history query";
        return 0;
    }
    Query* query=new Query;Query::State& q=*query->state;
    q.before=before;q.since=since;q.limit=limit;q.filterValue=filterValue;q.value=value;
    pthread_mutex_lock(&values_.mutex);
    if(!values_.initialized) {
        pthread_mutex_unlock(&values_.mutex);delete query;
        if(error)*error="history unavailable";
        return 0;
    }
    std::set<unsigned int> seen;
    for(size_t n=0;n<ids.size();++n) {
        std::string id=ids[n];
        if(id.find(':')==std::string::npos)id="zigbee:"+id;
        std::map<std::string,std::vector<unsigned int> >::const_iterator pair=values_.pairPages.find(pairKey(id,metric));
        if(pair==values_.pairPages.end())continue;
        for(size_t i=0;i<pair->second.size();++i) {
            unsigned int index=pair->second[i];const Page& page=values_.pages[index];
            if((!before||page.oldest<before)&&(!since||(page.newest>>18)>=since)&&seen.insert(index).second)
                q.pages.push_back(ReadPage(index,page));
        }
    }
    pthread_mutex_unlock(&values_.mutex);
    std::sort(q.pages.begin(),q.pages.end(),newestPageFirst);
    query->done=q.pages.empty();return query;
}
bool SensorHistory::stepQuery(Query* query,std::string* error,ReadStats* stats) const {
    if(query->done)return true;
    Query::State& q=*query->state;
    const ReadPage& snapshot=q.pages[q.position++];
    int fd=open(values_.path.c_str(),O_RDONLY);
    if(fd<0){if(error)*error="cannot read paged history";return false;}
    unsigned char header[fileHeaderBytes],bytes[pageBytes],confirmed[pageHeaderBytes];
    const off_t base=fileHeaderBytes+static_cast<off_t>(snapshot.index)*pageBytes;
    bool ok=pread(fd,header,sizeof(header),0)==fileHeaderBytes&&fileHeaderValid(header);
    if(ok) {
        if(stats){++stats->pagesRead;stats->bytesRead+=fileHeaderBytes+pageBytes+pageHeaderBytes;}
        ok=pread(fd,bytes,sizeof(bytes),base)==pageBytes&&
           pread(fd,confirmed,sizeof(confirmed),base)==pageHeaderBytes&&
           memcmp(bytes,confirmed,pageHeaderBytes)==0;
    }
    close(fd);
    std::string owner;unsigned char metric=0;uint32_t generation=0;
    if(!ok||!pageHeaderValid(bytes,&owner,&metric,&generation)||owner!=snapshot.page.id||
       metric!=snapshot.page.metric||generation!=snapshot.page.generation) {
        if(error)*error="history page changed or unreadable; retry query";
        return false;
    }
#ifdef MICROHAD_TESTING
    if(sensorHistoryQueryPageHook)sensorHistoryQueryPageHook(sensorHistoryQueryPageHookData);
#endif
    for(unsigned int i=0;i<snapshot.page.recordCount;++i) {
        const unsigned char* rec=bytes+pageHeaderBytes+i*recordBytes;
        uint32_t timestamp=get32(rec);uint64_t cursor=recordCursor(timestamp,snapshot.index,i);
        if(cursor<snapshot.page.oldest||cursor>snapshot.page.newest)continue;
        if(stats)++stats->recordsChecked;
        float value=decodeFloat(rec+4);
        if(rec[8]!=recordCrc(bytes,rec)||!timestamp||!std::isfinite(value)||
           (metric==AirQuality&&qualityText(value).empty())) {
            if(error)*error="invalid history record; retry query";
            return false;
        }
        if((q.before&&cursor>=q.before)||timestamp<q.since||(q.filterValue&&value!=q.value))continue;
        query->samples.push_back(ValueRecord(cursor,timestamp,metric,value));
    }
    std::sort(query->samples.begin(),query->samples.end(),newerRecord);
    if(query->samples.size()>q.limit)query->samples.resize(q.limit);
    query->done=q.position==q.pages.size()||
        (query->samples.size()==q.limit&&q.pages[q.position].page.newest<=query->samples.back().cursor);
    // Conservative continuation avoids an extra page read just to prove exhaustion.
    if(query->done&&query->samples.size()==q.limit)query->nextBefore=query->samples.back().cursor;
    return true;
}
