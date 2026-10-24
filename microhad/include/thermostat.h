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
    std::map<int, std::string> scheduleDetails;
    std::set<unsigned int> attributes;
    std::set<unsigned int> commands;
    bool attributesKnown;
    bool commandsKnown;
    bool scheduleSupported;
    std::set<std::string> pending;
    std::string sourceId;
    unsigned long staleMinutes;
    std::string status;
    std::string error;
    long sourceObservedAt;
    long lastForwardAt;
    double lastForwardValue;
    ThermostatState() : attributesKnown(false), commandsKnown(false), scheduleSupported(false), staleMinutes(90), sourceObservedAt(0), lastForwardAt(0), lastForwardValue(-1) {}
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
bool isThermostat(const Device& device);
double thermostatValue(const Device& device, const std::string& name, double fallback);
ThermostatAttribute thermostatDefinition(const Device& device, const ThermostatAttribute& base);
bool thermostatModeSupported(const Device& device, int mode);
bool thermostatScheduleForDevice(const Device& device, const std::string& text, int day, std::vector<unsigned char>* payload);
bool thermostatWireValue(unsigned int attribute, const std::string& kind, const std::string& value);
bool applyMatterThermostat(Device* device, unsigned int attribute, const std::string& kind, const std::string& value, long now);
bool isTrvzb(const Device& device);
bool thermostatSupported(const Device& device, const ThermostatAttribute& attribute);
bool thermostatNumber(const std::string& text, double* value);
bool thermostatEncode(const Device& device, const std::string& field, const std::string& text,
                      std::vector<unsigned char>* payload, std::string* error);
bool applyThermostatAttribute(Device* device, unsigned short cluster, unsigned short attribute,
                             unsigned char type, const std::vector<unsigned char>& value, long now);
bool thermostatSchedule(const std::string& text, int day, std::vector<unsigned char>* payload);
bool applyThermostatSchedule(Device* device, const std::vector<unsigned char>& payload);
// Pure validation shared by Lua draft validation and coordinator execution.
// Live network/queue state and source existence are checked by the caller.
bool thermostatValidateSetting(const Device& device, const std::string& field,
                               const std::string& value, std::string* error);
std::string thermostatJson(const Device& device);
#endif
