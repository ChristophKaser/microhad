#ifndef MICROHAD_ZIGBEE_SENSOR_HISTORY_H
#define MICROHAD_ZIGBEE_SENSOR_HISTORY_H

#include <map>
#include <pthread.h>
#include <set>
#include <string>
#include <sys/types.h>
#include <vector>

/* One bounded, page-organized binary history in RAM-backed /tmp. The
   coordinator is the sole writer; HTTP workers use the synchronized catalog
   snapshot to read only pages for a requested device. */
class SensorHistory {
public:
    enum { Temperature = 1, Humidity = 2, Contact = 3, Power = 4, Occupancy = 5, Battery = 6 };
    struct ReadStats {
        unsigned long pagesRead;
        unsigned long recordsChecked;
        unsigned long bytesRead;
        ReadStats() : pagesRead(0), recordsChecked(0), bytesRead(0) {}
    };

    explicit SensorHistory(const std::string& path);
    ~SensorHistory();
    const std::string& path() const;
    bool initialize(std::string* error = 0);
    bool append(const std::string& address, unsigned char metric, int value, long timestamp);
    void setActiveDevices(const std::vector<std::string>& addresses);
    bool setDevicePresent(const std::string& address, bool present);
    bool readForDevice(const std::string& address, unsigned long before,
                       std::string* json, std::string* error,
                       ReadStats* stats = 0) const;
    static bool readJson(const std::string& path, const std::string& address,
                         unsigned long before, std::string* json, std::string* error,
                         ReadStats* stats = 0);

private:
    struct PageInfo {
        unsigned long pageId;
        bool allocated;
        unsigned char address[8];
        unsigned char metric;
        unsigned long generation;
        unsigned int count;
        unsigned int nextSlot;
        unsigned long minimumSequence;
        unsigned long maximumSequence;
        PageInfo();
    };
    std::string path_;
    unsigned long sequence_;
    bool initialized_;
    mutable pthread_mutex_t catalogMutex_;
    std::vector<PageInfo> pages_;
    std::map<std::string, std::vector<unsigned long> > pairPages_;
    std::set<std::string> inactiveDevices_;
    std::map<std::string, long> lastMinute_;
    std::map<std::string, int> lastValue_;
    std::map<std::string, unsigned long> recordCount_;
    bool initializeV2(int fd, off_t size, std::string* error);
    bool readV2Json(const std::string& address, unsigned long before,
                    std::string* json, std::string* error, ReadStats* stats) const;
    bool allocatePage(const unsigned char* address, unsigned char metric,
                      unsigned long* pageId);
    bool writeNewFileHeader(int fd);
    void rebuildPairState(const std::string& key);
};

#endif
