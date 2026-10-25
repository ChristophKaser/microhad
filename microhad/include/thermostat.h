#ifndef MICROHAD_THERMOSTAT_H
#define MICROHAD_THERMOSTAT_H
#include <bitset>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../../protocol/generated/matter_messages.pb.h"
struct Device;
struct TemperatureObservation {
    long at;
    TemperatureObservation() : at(0) {}
};
// Attribute IDs and wire names share one definition; names are boundary data.
namespace ThermostatField {
enum Id {
#define THERMOSTAT_ATTRIBUTE(name, cluster, attribute, type, scale, minimum, maximum, step, writable, version) name,
#include "../../config/thermostat-attributes.def"
#undef THERMOSTAT_ATTRIBUTE
    Count
};
}
struct ThermostatValues {
    std::bitset<ThermostatField::Count> present;
    double numbers[ThermostatField::Count];
    ThermostatValues() { for(unsigned i=0;i<ThermostatField::Count;++i) numbers[i]=0; }
    bool count(ThermostatField::Id id) const { return id<ThermostatField::Count && present[id]; }
    double get(ThermostatField::Id id,double fallback) const { return count(id)?numbers[id]:fallback; }
    double& operator[](ThermostatField::Id id) { present.set(id); return numbers[id]; }
    void erase(ThermostatField::Id id) { if(id<ThermostatField::Count) present.reset(id); }
    bool operator==(const ThermostatValues& other) const {
        if(present!=other.present)return false;
        for(unsigned i=0;i<ThermostatField::Count;++i)if(present[i] && numbers[i]!=other.numbers[i])return false;
        return true;
    }
};
struct ThermostatFieldSet {
    std::bitset<ThermostatField::Count> bits;
    bool count(ThermostatField::Id id) const { return id<ThermostatField::Count && bits[id]; }
    void insert(ThermostatField::Id id) { if(id<ThermostatField::Count)bits.set(id); }
    void erase(ThermostatField::Id id) { if(id<ThermostatField::Count)bits.reset(id); }
    void clear() { bits.reset(); }
    bool operator==(const ThermostatFieldSet& other) const { return bits==other.bits; }
};
// Pending IDs: attributes, reporting configuration for each attribute, seven days.
enum { ThermostatPendingCount=2*ThermostatField::Count+7 };
unsigned thermostatPendingId(const std::string& name);
std::string thermostatPendingName(unsigned id);
struct ThermostatPending {
    std::bitset<ThermostatPendingCount> bits;
    bool count(unsigned id) const { return id<ThermostatPendingCount && bits[id]; }
    void insert(unsigned id) { if(id<ThermostatPendingCount)bits.set(id); }
    void erase(unsigned id) { if(id<ThermostatPendingCount)bits.reset(id); }
    void clear() { bits.reset(); }
    bool empty() const { return bits.none(); }
    bool operator==(const ThermostatPending& other) const { return bits==other.bits; }
};
struct ThermostatState {
    ThermostatValues values;
    ThermostatFieldSet unsupported;
    std::map<int, std::string> schedules;
    std::map<int, std::string> scheduleDetails;
    std::set<unsigned int> attributes;
    std::set<unsigned int> commands;
    bool attributesKnown;
    bool commandsKnown;
    bool scheduleSupported;
    ThermostatPending pending;
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
    ThermostatField::Id id;
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
const ThermostatAttribute* thermostatAttribute(ThermostatField::Id id);
bool isThermostat(const Device& device);
double thermostatValue(const Device& device, ThermostatField::Id id, double fallback);
ThermostatAttribute thermostatDefinition(const Device& device, const ThermostatAttribute& base);
bool thermostatModeSupported(const Device& device, int mode);
bool thermostatScheduleForDevice(const Device& device, const std::string& text, int day, std::vector<unsigned char>* payload);
bool thermostatWireValue(unsigned int attribute,const MatterProto::AttributeValue& value);
bool applyMatterThermostat(Device* device,unsigned int attribute,const MatterProto::AttributeValue& value,long now);
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
