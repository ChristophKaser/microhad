#ifndef MICROHAD_PROTOBUF_VALUE_H
#define MICROHAD_PROTOBUF_VALUE_H
#include <stdint.h>
#include <string>
#include <vector>
namespace Proto {
struct Value {
    enum Type { Null, Bool, UInt, Int, Float, Text, Binary, Array, Map } type;
    uint64_t u;
    int64_t i;
    double f;
    std::string text;
    std::vector<Value> items;
    std::vector<std::string> keys;
    Value() : type(Null), u(0), i(0), f(0) {}
};
typedef std::vector<unsigned char> Bytes;
const char* requestName(unsigned opcode);
const char* ackName(unsigned opcode);
const Value* field(const Value&, const std::string&);
void add(Value*, const std::string&, const Value&);
bool encode(const std::string&, const Value&, Bytes*, size_t limit);
bool decode(const std::string&, const Bytes&, Value*, size_t limit, unsigned int* unknown);
bool normalize(Value*, const std::string&, unsigned kind, unsigned opcode, const std::string& event, bool incoming);
}
#endif
