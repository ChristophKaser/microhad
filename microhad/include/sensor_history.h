#ifndef MICROHAD_ZIGBEE_SENSOR_HISTORY_H
#define MICROHAD_ZIGBEE_SENSOR_HISTORY_H

#include <map>
#include <stdint.h>
#include <pthread.h>
#include <string>
#include <sys/types.h>
#include <vector>

/* Bounded paged sensor history in RAM-backed /tmp. */
class SensorHistory {
public:
    enum { Temperature = 1, Humidity = 2, Contact = 3, Power = 4, Occupancy = 5, Battery = 6,
           CarbonDioxide = 7, Pm25 = 8, AirQuality = 9, Light = 10, Brightness = 11,
           CarbonDioxideMass = 12, CarbonDioxideParticles = 13, CarbonDioxideBecquerels = 14,
           CarbonDioxideDecibels = 15, Pm25Ppm = 16, Pm25Particles = 17,
           Pm25Becquerels = 18, Pm25Decibels = 19 };
    struct ReadStats {
        unsigned long pagesRead;
        unsigned long recordsChecked;
        unsigned long bytesRead;
        ReadStats() : pagesRead(0), recordsChecked(0), bytesRead(0) {}
    };

    explicit SensorHistory(const std::string& path, const std::string& flashPath = "");
    ~SensorHistory();
    const std::string& path() const;
    bool initialize(std::string* error = 0);
    std::string takePersistenceError();
#ifdef MICROHAD_TESTING
    void useManualPersistenceForTests();
    bool persistForTests(unsigned long seconds, std::string* error);
    unsigned long persistenceWritesForTests() const;
#endif
    bool appendValue(const std::string& deviceId, unsigned char metric, const std::string& value,
                     const std::string& unit, long timestamp);
    bool readForDevice(const std::string& deviceId, uint64_t before,
                       std::string* json, std::string* error,
                       ReadStats* stats = 0) const;
    static bool readJson(const std::string& path, const std::string& deviceId,
                         uint64_t before, std::string* json, std::string* error,
                         ReadStats* stats = 0);

public:
    struct ValueRecord {
        uint64_t cursor;
        unsigned long timestamp;
        float value;
        unsigned char metric;
        ValueRecord(uint64_t c = 0, unsigned long t = 0, unsigned char m = 0, float v = 0)
            : cursor(c), timestamp(t), value(v), metric(m) {}
    };
    struct Page {
        bool allocated;
        std::string id;
        unsigned char metric;
        unsigned long generation;
        uint64_t newest;
        uint64_t oldest;
        unsigned long lastTimestamp;
        float lastValue;
        unsigned int recordCount;
        Page() : allocated(false), metric(0), generation(0), newest(0), oldest(0), lastTimestamp(0), lastValue(0), recordCount(0) {}
    };
    struct LastValue {
        unsigned long generation;
        unsigned long timestamp;
        float value;
        LastValue(unsigned long s = 0, unsigned long t = 0, float v = 0) : generation(s), timestamp(t), value(v) {}
    };
private:
    struct ValueJournal {
        std::string path;
        unsigned long generation;
        bool initialized;
        mutable pthread_mutex_t mutex;
        std::vector<Page> pages;
        std::map<std::string, std::vector<unsigned int> > pairPages;
        std::map<std::string, LastValue> lastValues;
        ValueJournal();
        ~ValueJournal();
    };
    struct FlashState;
    FlashState* flash_;
    ValueJournal values_;
    bool prepareFlash(std::string* error);
    bool persistPages(unsigned long seconds, std::string* error);
    static void* persistenceThread(void* opaque);
    void runPersistence();
    bool initializeValues(std::string* error);
    bool appendValueLocked(const std::string& deviceId, unsigned char metric, const std::string& value,
                           const std::string& unit, long timestamp);
    bool readValuesJson(const std::string& deviceId, uint64_t before,
                        std::string* json, std::string* error, ReadStats* stats) const;
};

#endif
