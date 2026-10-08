#ifndef MICROHAD_ZIGBEE_SENSOR_HISTORY_H
#define MICROHAD_ZIGBEE_SENSOR_HISTORY_H

#include <map>
#include <pthread.h>
#include <string>
#include <sys/types.h>
#include <vector>

/* One bounded source-neutral history journal in RAM-backed /tmp. */
class SensorHistory {
public:
    enum { Temperature = 1, Humidity = 2, Contact = 3, Power = 4, Occupancy = 5, Battery = 6,
           CarbonDioxide = 7, Pm25 = 8, AirQuality = 9, Light = 10, Brightness = 11 };
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
    bool appendValue(const std::string& deviceId, unsigned char metric, const std::string& value,
                     const std::string& unit, long timestamp);
    bool readForDevice(const std::string& deviceId, unsigned long before,
                       std::string* json, std::string* error,
                       ReadStats* stats = 0) const;
    static bool readJson(const std::string& path, const std::string& deviceId,
                         unsigned long before, std::string* json, std::string* error,
                         ReadStats* stats = 0);

private:
    struct ValueRecord {
        unsigned long sequence;
        off_t offset;
        ValueRecord(unsigned long seq = 0, off_t at = 0) : sequence(seq), offset(at) {}
    };
    struct ValueJournal {
        std::string path;
        unsigned long sequence;
        unsigned long recordCount;
        bool initialized;
        mutable pthread_mutex_t mutex;
        std::map<std::string, std::vector<ValueRecord> > records;
        std::map<std::string, std::string> lastValues;
        std::map<std::string, long> lastMinutes;
        ValueJournal();
        ~ValueJournal();
    };
    std::string obsoleteHistoryPath_;
    ValueJournal values_;
    bool initializeValues(std::string* error);
    bool appendValueLocked(const std::string& deviceId, unsigned char metric, const std::string& value,
                           const std::string& unit, long timestamp);
    bool readValuesJson(const std::string& deviceId, unsigned long before,
                        std::string* json, std::string* error, ReadStats* stats) const;
};

#endif
