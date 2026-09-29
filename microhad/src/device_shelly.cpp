#include "device_shelly.h"

#include <cstdlib>

namespace {
bool jsonBoolField(const std::string& object, const char* name, bool* value) {
    std::string key = std::string("\"") + name + "\"";
    std::string::size_type at = object.find(key);
    if (at == std::string::npos) return false;
    at = object.find(':', at + key.size());
    if (at == std::string::npos) return false;
    ++at;
    while (at < object.size() && (object[at] == ' ' || object[at] == '\t')) ++at;
    if (object.compare(at, 4, "true") == 0) { *value = true; return true; }
    if (object.compare(at, 5, "false") == 0) { *value = false; return true; }
    return false;
}

std::string firstObject(const std::string& input, const std::string& key) {
    std::string::size_type at = input.find(std::string("\"") + key + "\"");
    if (at == std::string::npos || (at = input.find('[', at)) == std::string::npos || (at = input.find('{', at)) == std::string::npos) return "";
    std::string::size_type start = at;
    int depth = 0; bool quoted = false, escaped = false;
    for (; at < input.size(); ++at) {
        char c = input[at];
        if (quoted) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; }
        else if (c == '"') quoted = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return input.substr(start, at - start + 1);
    }
    return "";
}
}

namespace ShellyProtocol {
bool parsePowerStatus(const std::string& body, double* watts) {
    std::string meter = firstObject(body, "meters");
    std::string::size_type powerAt = meter.find("\"power\"");
    bool reportedValid = true;
    if (meter.empty() || powerAt == std::string::npos ||
        (meter.find("\"is_valid\"") != std::string::npos &&
         (!jsonBoolField(meter, "is_valid", &reportedValid) || !reportedValid))) return false;
    powerAt = meter.find(':', powerAt + 7);
    if (powerAt == std::string::npos) return false;
    char* end = 0;
    double value = strtod(meter.c_str() + powerAt + 1, &end);
    if (end == meter.c_str() + powerAt + 1) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if ((*end != ',' && *end != '}') || value != value || value < 0 || value > 6553.5) return false;
    *watts = value;
    return true;
}
}
