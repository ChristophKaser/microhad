#ifndef MICROHAD_AUTOMATION_TYPES_H
#define MICROHAD_AUTOMATION_TYPES_H
#include <map>
#include <string>
#include <vector>
#include <stdint.h>
#include <tr1/memory>
struct Device;

namespace Automation {
struct Value {
    enum Type { Null, Boolean, Number, String, Object, Array } type;
    bool boolean;
    double number;
    std::string string;
    std::map<std::string, Value> fields;
    std::vector<Value> items;
    Value() : type(Null), boolean(false), number(0) {}
    explicit Value(bool v) : type(Boolean), boolean(v), number(0) {}
    explicit Value(double v) : type(Number), boolean(false), number(v) {}
    explicit Value(const std::string& v) : type(String), boolean(false), number(0), string(v) {}
    explicit Value(const char* v) : type(String), boolean(false), number(0), string(v) {}
    static Value object() { Value v; v.type=Object; return v; }
    static Value array() { Value v; v.type=Array; return v; }
    bool operator==(const Value& v) const;
    bool operator!=(const Value& v) const { return !(*this==v); }
};
std::string json(const Value& value);
bool parse(const std::string& text, Value* value);
Value snapshot(const Device& device);
typedef std::tr1::shared_ptr<const Device> DevicePtr;

// Typed immutable snapshots cross threads; map-shaped Lua values are built only
// when a consumer needs them. Unchanged device metadata is shared across versions.
enum Field { ActorOn, AirQuality, Battery, Brightness, Co2, ContactOpen, Humidity,
    Id, Kind, LastSeen, LightOn, LinkQuality, Name, Occupied, Online, Pm25, Power,
    Source, Temperature, WaterDetected, WifiSignal, FieldCount };
struct DeviceMetadata {
    std::string id,name,lookupName,source,kind,matterNode;
    int matterEndpoint;
    bool light,actor,brightness,contact,humidity;
    explicit DeviceMetadata(const Device& device);
    bool matches(const Device& device) const;
};
typedef std::tr1::shared_ptr<const DeviceMetadata> MetadataPtr;
struct DeviceSnapshot {
    DevicePtr record;
    MetadataPtr metadata;
    uint32_t present;
    double values[FieldCount];
    std::string airQuality;
    DeviceSnapshot();
    bool has(Field field) const { return (present & (1U<<field))!=0; }
};
typedef std::tr1::shared_ptr<const DeviceSnapshot> SnapshotPtr;
SnapshotPtr capture(const Device& device,const SnapshotPtr& previous=SnapshotPtr(),const DevicePtr& record=DevicePtr());
bool sameField(const DeviceSnapshot& a,const DeviceSnapshot& b,Field field);
Value fieldValue(const DeviceSnapshot& snapshot,Field field);
const char* fieldName(Field field);
Field findField(const std::string& name);
Value snapshot(const DeviceSnapshot& device);
struct SceneEntry { unsigned char id;std::string name; };
struct SceneCatalogEntry { unsigned short id;std::string name;std::vector<SceneEntry> scenes; };
typedef std::vector<SceneCatalogEntry> SceneCatalog;
typedef std::map<std::string,std::string> NameIndex;
typedef std::vector<SnapshotPtr> SnapshotList;
typedef std::tr1::shared_ptr<const SnapshotList> SnapshotListPtr;
struct Catalog {
    SnapshotListPtr ordered;
    std::map<std::string,SnapshotPtr> devices;
    std::tr1::shared_ptr<const NameIndex> names;
    std::tr1::shared_ptr<const SceneCatalog> groups;
    Catalog();
    const DeviceSnapshot* resolve(const std::string& target,std::string* error) const;
};
typedef std::tr1::shared_ptr<const Catalog> CatalogPtr;

struct Event {
    Value data;
    SnapshotPtr device;
    std::string type,action;
    Field field;
    Value old;
    bool hasOld;
    double at;
    int endpoint,position,pressCount;
    unsigned int depth;
    uint64_t chain;
    Event() : data(Value::object()), field(FieldCount), hasOld(false), at(0), endpoint(0), position(-1), pressCount(-1), depth(0), chain(0) {}
};
Value eventValue(const Event& event);
struct Action {
    std::string operation, target, script;
    unsigned long generation;
    Value args;
    uint64_t chain;
    unsigned int depth;
    Action() : generation(0), args(Value::object()), chain(0), depth(0) {}
};
}
#endif
