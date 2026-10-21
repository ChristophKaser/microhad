#ifndef MICROHAD_THERMOSTAT_H
#define MICROHAD_THERMOSTAT_H
#include <map>
#include <set>
#include <string>
#include <vector>
struct Device;
struct TemperatureObservation {
    long at;
    TemperatureObservation() : at(0) {}
};
struct ThermostatState {
    std::map<std::string, double> values;
    std::set<std::string> unsupported;
    std::map<int, std::string> schedules;
    std::set<std::string> pending;
    std::string sourceId;
    unsigned long staleMinutes;
    std::string status;
    std::string error;
    long sourceObservedAt;
    long lastForwardAt;
    double lastForwardValue;
    ThermostatState() : staleMinutes(90), sourceObservedAt(0), lastForwardAt(0), lastForwardValue(-1) {}
};
struct ThermostatAttribute {
    const char* name;
    unsigned short cluster;
    unsigned short attribute;
    unsigned char type;
    double scale;
    double minimum;
    double maximum;
    double step;
    bool writable;
    const char* minimumVersion;
};
const ThermostatAttribute* thermostatAttributes(unsigned long* count);
const ThermostatAttribute* thermostatAttribute(const std::string& name);
bool isTrvzb(const Device& device);
bool thermostatSupported(const Device& device, const ThermostatAttribute& attribute);
bool thermostatNumber(const std::string& text, double* value);
bool thermostatEncode(const Device& device, const std::string& field, const std::string& text,
                      std::vector<unsigned char>* payload, std::string* error);
bool applyThermostatAttribute(Device* device, unsigned short cluster, unsigned short attribute,
                             unsigned char type, const std::vector<unsigned char>& value, long now);
bool thermostatSchedule(const std::string& text, int day, std::vector<unsigned char>* payload);
bool applyThermostatSchedule(Device* device, const std::vector<unsigned char>& payload);
std::string thermostatJson(const Device& device);
#endif
