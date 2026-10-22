#ifndef MICROHAD_AUTOMATION_TYPES_H
#define MICROHAD_AUTOMATION_TYPES_H
#include <map>
#include <string>
#include <vector>
#include <stdint.h>
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
struct Event {
    Value data;
    unsigned int depth;
    uint64_t chain;
    Event() : data(Value::object()), depth(0), chain(0) {}
};
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
