#ifndef MICROHAD_ZIGBEE_SENSOR_HISTORY_H
#define MICROHAD_ZIGBEE_SENSOR_HISTORY_H

#include <map>
#include <string>

/* One bounded binary ring in RAM-backed /tmp. The web workers only read the
   file; the coordinator is its sole writer. */
class SensorHistory {
public:
    enum { Temperature = 1, Humidity = 2, Contact = 3 };
    explicit SensorHistory(const std::string& path);
    const std::string& path() const;
    bool append(const std::string& address, unsigned char metric, int value, long timestamp);
    static bool readJson(const std::string& path, const std::string& address,
                         unsigned long before, std::string* json, std::string* error);
private:
    std::string path_;
    unsigned long sequence_;
    bool initialized_;
    std::map<std::string, long> lastMinute_;
    bool initialize();
};

#endif
