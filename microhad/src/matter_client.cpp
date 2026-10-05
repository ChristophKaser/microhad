#include "matter_client.h"
#include "matter_protocol.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sstream>
#include <time.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
const unsigned int ioTimeoutSeconds = 1;

typedef std::map<std::string, std::string> Fields;
// Bounded JSON syntax; operation-specific validators restrict every field.
// Arrays are bounded and used only by typed device snapshot replies.
class StatusJson {
public:
    explicit StatusJson(const std::string& text) : text_(text), at_(0) {}
    bool parse(Fields* fields) { return object(fields, 0) && whitespace() == text_.size(); }
    bool parseArray(std::vector<std::string>* values) { return array(values, 0) && whitespace() == text_.size(); }
private:
    const std::string& text_;
    unsigned long at_;
    unsigned long whitespace() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\n' ||
               text_[at_] == '\r' || text_[at_] == '\t')) ++at_;
        return at_;
    }
    bool take(char c) {
        whitespace();
        if (at_ >= text_.size() || text_[at_] != c) return false;
        ++at_; return true;
    }
    bool string(std::string* value) {
        if (!take('"')) return false;
        value->clear();
        while (at_ < text_.size() && text_[at_] != '"') {
            unsigned char c = text_[at_++];
            if (c < 0x20 || c > 0x7e || c == '\\' || value->size() >= 508) return false;
            *value += static_cast<char>(c);
        }
        return take('"');
    }
    bool array(std::vector<std::string>* values, unsigned int depth) {
        if (depth > 4 || !take('[')) return false;
        whitespace();
        if (at_ < text_.size() && text_[at_] == ']') { ++at_; return true; }
        while (values->size() < 64) {
            unsigned long start = whitespace();
            if (at_ >= text_.size()) return false;
            if (text_[at_] == '{') {
                Fields nested;
                if (!object(&nested, depth + 1)) return false;
            } else {
                while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
                if (at_ == start || at_ - start > 10 || (at_ - start > 1 && text_[start] == '0')) return false;
            }
            values->push_back(text_.substr(start, at_ - start));
            whitespace();
            if (at_ < text_.size() && text_[at_] == ']') { ++at_; return true; }
            if (!take(',')) return false;
        }
        return false;
    }
    bool object(Fields* fields, unsigned int depth) {
        if (depth > 4 || !take('{')) return false;
        whitespace();
        if (at_ < text_.size() && text_[at_] == '}') { ++at_; return true; }
        while (fields->size() < 12) {
            std::string key, value;
            if (!string(&key) || fields->count(key) || !take(':')) return false;
            whitespace();
            if (at_ >= text_.size()) return false;
            if (text_[at_] == '{') {
                unsigned long start = at_;
                Fields nested;
                if (!object(&nested, depth + 1)) return false;
                value = text_.substr(start, at_ - start);
            } else if (text_[at_] == '[') {
                unsigned long start = at_;
                std::vector<std::string> values;
                if (!array(&values, depth + 1)) return false;
                value = text_.substr(start, at_ - start);
            } else if (text_[at_] == '"') {
                if (!string(&value)) return false;
                value = '"' + value + '"';
            } else {
                unsigned long start = at_;
                while (at_ < text_.size() && text_[at_] != ',' && text_[at_] != '}' &&
                       text_[at_] != ' ' && text_[at_] != '\t' && text_[at_] != '\r' && text_[at_] != '\n') ++at_;
                value = text_.substr(start, at_ - start);
                if (value != "true" && value != "false" && value != "null" && !jsonNumber(value)) return false;
            }
            (*fields)[key] = value;
            whitespace();
            if (at_ < text_.size() && text_[at_] == '}') { ++at_; return true; }
            if (!take(',')) return false;
        }
        return false;
    }
    static bool jsonNumber(const std::string& value) {
        if (value.empty() || value.size() > 32) return false;
        unsigned long i = value[0] == '-' ? 1 : 0;
        if (i == value.size()) return false;
        if (value[i] == '0') ++i;
        else {
            if (value[i] < '1' || value[i] > '9') return false;
            while (i < value.size() && value[i] >= '0' && value[i] <= '9') ++i;
        }
        if (i < value.size() && value[i] == '.') {
            ++i; unsigned long start = i;
            while (i < value.size() && value[i] >= '0' && value[i] <= '9') ++i;
            if (i == start) return false;
        }
        if (i < value.size() && (value[i] == 'e' || value[i] == 'E')) {
            ++i; if (i < value.size() && (value[i] == '+' || value[i] == '-')) ++i;
            unsigned long start = i;
            while (i < value.size() && value[i] >= '0' && value[i] <= '9') ++i;
            if (i == start) return false;
        }
        return i == value.size();
    }
};

bool booleanField(const Fields& fields, const char* name) {
    Fields::const_iterator it = fields.find(name);
    return it != fields.end() && (it->second == "true" || it->second == "false");
}
bool member(const std::string& value, const char* const* values, unsigned int count) {
    for (unsigned int i = 0; i < count; ++i) if (value == values[i]) return true;
    return false;
}
bool snapshotValid(const Fields& fields, bool thread) {
    static const char* const common[] = {"enabled", "available", "state"};
    static const char* const matterOnly[] = {"controller_initialized", "fabric_ready"};
    static const char* const threadOnly[] = {"configured", "attached", "linux_ipv6_or_tun", "zigbee_channel", "dataset_channel"};
    static const char* const states[] = {"\"awaiting_coordinator\"", "\"detached\"", "\"attaching\"", "\"attached\"", "\"channel_mismatch\"", "\"error\""};
    if (!booleanField(fields, "enabled") || !booleanField(fields, "available") || !fields.count("state")) return false;
    if (thread) {
        if (!booleanField(fields, "configured") || !booleanField(fields, "attached") ||
            !fields.count("linux_ipv6_or_tun") || fields.find("linux_ipv6_or_tun")->second != "false" ||
            !member(fields.find("state")->second, states, 6)) return false;
    } else if (!booleanField(fields, "controller_initialized") || !booleanField(fields, "fabric_ready") ||
               (fields.find("state")->second != "\"running\"" && fields.find("state")->second != "\"awaiting_coordinator\"")) return false;
    for (Fields::const_iterator it = fields.begin(); it != fields.end(); ++it) {
        if (!member(it->first, common, 3) && !member(it->first, thread ? threadOnly : matterOnly, thread ? 5 : 2)) return false;
        if (it->first == "zigbee_channel" || it->first == "dataset_channel") {
            if (it->second.size() != 2 || it->second[0] < '1' || it->second[0] > '2' ||
                it->second[1] < '0' || it->second[1] > '9') return false;
            int channel = (it->second[0] - '0') * 10 + it->second[1] - '0';
            if (channel < 11 || channel > 26) return false;
        }
    }
    return true;
}
bool number(const std::string& value, uint32_t minimum, uint32_t maximum) {
    if (value.empty() || value.size() > 10 || (value.size() > 1 && value[0] == '0')) return false;
    uint64_t result = 0;
    for (unsigned long i = 0; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
        result = result * 10 + value[i] - '0';
    }
    return result >= minimum && result <= maximum;
}
bool finiteNumber(const std::string& value, double minimum, double maximum) {
    if (value == "null") return true;
    if (value.empty() || value.size() > 32) return false;
    char* end = 0;
    const double parsed = std::strtod(value.c_str(), &end);
    return end == value.c_str() + value.size() && std::isfinite(parsed) && parsed >= minimum && parsed <= maximum;
}
bool measurementValueValid(const std::string& text, uint32_t cluster) {
    Fields measurement;
    if (!StatusJson(text).parse(&measurement) || measurement.size() != 3 ||
        !measurement.count("value") || !measurement.count("unit") || !measurement.count("quality")) return false;
    const bool unknown = measurement["value"] == "null";
    if (measurement["quality"] != (unknown ? "\"unknown\"" : "\"measured\"")) return false;
    if (cluster == 1026) return measurement["unit"] == "\"celsius\"" &&
        finiteNumber(measurement["value"], -327.68, 327.67);
    if (cluster == 1029) return measurement["unit"] == "\"percent\"" &&
        finiteNumber(measurement["value"], 0.0, 100.0);
    static const char* const units[] = {"\"ppm\"", "\"ppb\"", "\"ppt\"", "\"mg/m3\"", "\"ug/m3\"",
        "\"ng/m3\"", "\"particles/m3\"", "\"Bq/m3\"", "\"dB/m\"", "\"particles/ft3\""};
    return (cluster == 1037 || cluster == 1066) && member(measurement["unit"], units, 10) &&
        finiteNumber(measurement["value"], 0.0, 3.402823466e38);
}
bool typedValueValid(const Fields& result, Fields value) {
    const std::string& clusterName = result.find("cluster")->second;
    if (clusterName == "6" || clusterName == "69")
        return value.size() == 2 && value["type"] == "\"bool\"" && booleanField(value, "value");
    if (clusterName == "8") return value.size() == 2 && value["type"] == "\"u8\"" &&
        (value["value"] == "null" || number(value["value"], 0, 254));
    uint32_t cluster = 0;
    if (clusterName == "91") cluster = 91;
    else if (clusterName == "1026") cluster = 1026;
    else if (clusterName == "1029") cluster = 1029;
    else if (clusterName == "1037") cluster = 1037;
    else if (clusterName == "1066") cluster = 1066;
    else return false;
    if (value.size() != 2 || !value.count("type") || !value.count("value")) return false;
    if (cluster != 91) return value["type"] == "\"measurement\"" && measurementValueValid(value["value"], cluster);
    static const char* const qualities[] = {"\"unknown\"", "\"measured\""};
    static const char* const levels[] = {"\"unknown\"", "\"good\"", "\"fair\"", "\"moderate\"",
        "\"poor\"", "\"very_poor\"", "\"extremely_poor\""};
    Fields air;
    return value["type"] == "\"air_quality\"" && StatusJson(value["value"]).parse(&air) &&
        air.size() == 2 && member(air["value"], levels, 7) && member(air["quality"], qualities, 2) &&
        ((air["value"] == "\"unknown\"") == (air["quality"] == "\"unknown\""));
}
bool hex(const std::string& value, unsigned int minimum, unsigned int maximum) {
    if (value.size() < minimum + 2 || value.size() > maximum + 2 ||
        value[0] != '"' || value[value.size()-1] != '"' || (value.size() % 2)) return false;
    for (unsigned long i = 1; i + 1 < value.size(); ++i)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
    return true;
}
bool errorValid(const std::string& value) {
    static const char* const errors[] = {"\"malformed\"", "\"not_ready\"", "\"busy\"", "\"stale_session\"",
        "\"stale_generation\"", "\"timeout\"", "\"cancelled\"", "\"transport\"", "\"storage\"",
        "\"capacity\"", "\"accessory\"", "\"unsupported\"", "\"not_found\""};
    return member(value, errors, 13);
}
bool sessionValid(Fields fields) {
    static const char* const states[] = {"\"waiting_ble\"", "\"pase\"", "\"provisioning\"", "\"waiting_thread\"",
        "\"case\"", "\"completing\"", "\"complete\"", "\"cancelled\"", "\"failed\"", "\"timed_out\""};
    unsigned int count = 3;
    if (fields.count("error")) { ++count; if (!errorValid(fields["error"]) || fields["error"] == "\"unsupported\"") return false; }
    if (fields.count("node")) { ++count; if (!hex(fields["node"], 16, 16) || fields["node"] == "\"0000000000000000\"") return false; }
    return fields.size() == count && hex(fields["session"], 16, 16) &&
        fields["session"] != "\"0000000000000000\"" && member(fields["state"], states, 10) &&
        number(fields["deadline_remaining_seconds"], 0, 900);
}
bool devicePageValid(Fields fields) {
    std::vector<std::string> devices;
    if (fields.size() != 3 || !hex(fields["revision"], 16, 16) ||
        fields["revision"] == "\"0000000000000000\"" ||
        (fields["next_cursor"] != "null" && !number(fields["next_cursor"], 1, 2047)) ||
        !StatusJson(fields["devices"]).parseArray(&devices) || devices.size() > 1) return false;
    if (devices.empty()) return fields["next_cursor"] == "null";
    Fields device;
    if (!StatusJson(devices[0]).parse(&device) || device.size() != 6 ||
        !hex(device["node"], 16, 16) || device["node"] <= "\"0000000000000001\"" ||
        !booleanField(device, "online")) return false;
    static const char* const states[] = {"\"complete\"", "\"recovery_required\"", "\"commissioning\""};
    if (!member(device["state"], states, 3) ||
        (device["online"] == "true" && device["state"] != "\"complete\"")) return false;
    std::vector<std::string> clusters;
    if (!StatusJson(device["servers"]).parseArray(&clusters)) return false;
    if (device["endpoint"] == "null") return device["id"] == "null" &&
        device["online"] == "false" && clusters.empty();
    if (!number(device["endpoint"], 0, 65534)) return false;
    const std::string id = device["id"];
    if (id.size() != 47 || id.substr(0, 8) != "\"matter:" || id[24] != ':' || id[41] != ':' ||
        id[46] != '\"' || !hex('\"' + id.substr(8, 16) + '\"', 16, 16) ||
        id.substr(8, 16) == "0000000000000000" || id.substr(25, 16) != device["node"].substr(1, 16) ||
        !hex('\"' + id.substr(42, 4) + '\"', 4, 4)) return false;
    unsigned int endpoint = 0;
    for (unsigned long i = 42; i < 46; ++i) endpoint = endpoint * 16 +
        (id[i] <= '9' ? id[i] - '0' : id[i] - 'a' + 10);
    std::ostringstream canonical; canonical << endpoint;
    if (canonical.str() != device["endpoint"]) return false;
    uint64_t previous = 0; bool descriptor = false;
    for (unsigned long i = 0; i < clusters.size(); ++i) {
        if (!number(clusters[i], 0, 0xffffffffU)) return false;
        uint64_t value = 0;
        for (unsigned long j = 0; j < clusters[i].size(); ++j) value = value * 10 + clusters[i][j] - '0';
        if (i != 0 && value <= previous) return false;
        previous = value; if (value == 29) descriptor = true;
    }
    return descriptor;
}
bool operationalJobValid(Fields fields) {
    if (!fields.count("state") || !hex(fields["job"], 16, 16) || fields["job"] == "\"0000000000000000\"") return false;
    if (fields["state"] == "\"queued\"" || fields["state"] == "\"running\"") return fields.size() == 2;
    if (fields["state"] == "\"failed\"") return fields.size() == 3 && errorValid(fields["error"]);
    if (fields["state"] != "\"complete\"" || fields.size() != 3) return false;
    Fields result, value;
    if (!StatusJson(fields["result"]).parse(&result) || result.size() != 5 ||
        !hex(result["node"], 16, 16) || result["node"] == "\"0000000000000000\"" ||
        !number(result["endpoint"], 0, 65534) || result["attribute"] != "0" ||
        !StatusJson(result["value"]).parse(&value) || value.size() != 2) return false;
    return typedValueValid(result, value);
}
bool acceptedOperationValid(Fields fields) {
    return fields.size() == 2 && hex(fields["job"], 16, 16) &&
        fields["job"] != "\"0000000000000000\"" && fields["state"] == "\"queued\"";
}
bool responseValid(unsigned char opcode, const std::string& text) {
    Fields fields;
    if (!StatusJson(text).parse(&fields)) return false;
    if (opcode == 1 || opcode == 2) return snapshotValid(fields, opcode == 2);
    if (opcode < 3 || opcode > 11 || fields.size() != 3 || fields["schema"] != "1" || !booleanField(fields, "ok")) return false;
    if (fields["ok"] == "false") return fields.count("error") && (errorValid(fields["error"]) ||
        (opcode == 10 && fields["error"] == "\"stale_revision\""));
    Fields result;
    if (!fields.count("result") || !StatusJson(fields["result"]).parse(&result)) return false;
    if (opcode == 9) return snapshotValid(result, true);
    if (opcode == 10) return devicePageValid(result) || operationalJobValid(result) || acceptedOperationValid(result);
    if (opcode == 11) return acceptedOperationValid(result);
    if (opcode <= 5) return sessionValid(result);
    const char* name = opcode == 6 ? "generation" : opcode == 7 ? "sequence" : "write";
    return result.size() == 1 && number(result[name], 1, 0xffffffffU);
}
bool requestValid(unsigned char opcode, const std::string& text) {
    Fields fields;
    if (text.size() > MatterWire::MaxPayload || !StatusJson(text).parse(&fields) || fields["schema"] != "1") return false;
    if (opcode == 10) {
        if (fields["action"] == "\"read\"") return fields.size() == 6 &&
            hex(fields["node"], 16, 16) && fields["node"] > "\"0000000000000001\"" &&
            number(fields["endpoint"], 0, 65534) &&
            (fields["cluster"] == "6" || fields["cluster"] == "8" || fields["cluster"] == "69" || fields["cluster"] == "91" ||
             fields["cluster"] == "1026" || fields["cluster"] == "1029" ||
             fields["cluster"] == "1037" || fields["cluster"] == "1066") && fields["attribute"] == "0";
        if (fields["action"] == "\"job\"") return fields.size() == 3 &&
            hex(fields["job"], 16, 16) && fields["job"] != "\"0000000000000000\"";
        if (fields["action"] != "\"snapshot\"" ||
            (fields.count("cursor") && !number(fields["cursor"], 0, 2047)) ||
            (fields.count("revision") && (!hex(fields["revision"], 16, 16) || fields["revision"] == "\"0000000000000000\"")) ||
            (fields.count("cursor") && fields["cursor"] != "0" && !fields.count("revision"))) return false;
        return fields.size() == 2 + fields.count("cursor") + fields.count("revision");
    }
    if (opcode == 11) {
        static const char* const commands[] = {"\"on\"", "\"off\"", "\"toggle\""};
        if (!hex(fields["node"], 16, 16) || fields["node"] <= "\"0000000000000001\"" ||
            !number(fields["endpoint"], 0, 65534)) return false;
        if (member(fields["command"], commands, 3)) return fields.size() == 4;
        return fields["command"] == "\"move_to_level\"" && fields.size() == 6 &&
            number(fields["level"], 0, 254) && number(fields["transition_deciseconds"], 0, 65535);
    }
    if (opcode == 9) {
        if (fields["action"] == "\"create\"") return fields.size() == 2;
        if (fields["action"] == "\"retry\"") return fields.size() == 2;
        return fields["action"] == "\"import\"" && fields.size() == 3 && hex(fields["dataset"], 2, 508);
    }
    if (opcode == 3) {
        static const char* const excluded[] = {"11111111", "22222222", "33333333", "44444444", "55555555",
            "66666666", "77777777", "88888888", "12345678", "87654321"};
        return fields.size() == 4 && number(fields["discriminator"], 0, 4095) &&
            number(fields["pin"], 1, 99999998) && !member(fields["pin"], excluded, 10) &&
            number(fields["timeout_seconds"], 30, 900);
    }
    if (opcode < 4 || opcode > 8 || !hex(fields["session"], 16, 16) ||
        fields["session"] == "\"0000000000000000\"") return false;
    if (opcode == 4) return fields.size() == (fields.count("subscribe") ? 3U : 2U) &&
        (!fields.count("subscribe") || booleanField(fields, "subscribe"));
    if (opcode == 5) return fields.size() == 2;
    if (opcode == 6) return fields.size() == 4 && hex(fields["address"], 12, 12) && number(fields["mtu"], 23, 517);
    if (!number(fields["generation"], 1, 0xffffffffU)) return false;
    if (opcode == 7) return fields.size() == 5 && number(fields["sequence"], 1, 0xffffffffU) && hex(fields["data"], 2, 488);
    return fields.size() == 5 && number(fields["write"], 1, 0xffffffffU) && booleanField(fields, "success");
}

bool matterReadingFromReport(const Fields& data, MatterReading* reading) {
    if (!reading || data.size() != 5 || !data.count("node") || !data.count("endpoint") ||
        !data.count("cluster") || !data.count("attribute") || !data.count("value") ||
        !hex(data.find("node")->second, 16, 16) ||
        data.find("node")->second <= "\"0000000000000001\"" ||
        !number(data.find("endpoint")->second, 0, 65534) ||
        !number(data.find("attribute")->second, 0, 0)) return false;
    const std::string& clusterText = data.find("cluster")->second;
    uint32_t cluster = 0;
    if (number(clusterText, 6, 6)) cluster = 6;
    else if (number(clusterText, 69, 69)) cluster = 69;
    else if (number(clusterText, 8, 8)) cluster = 8;
    else if (number(clusterText, 91, 91)) cluster = 91;
    else if (number(clusterText, 1026, 1026)) cluster = 1026;
    else if (number(clusterText, 1029, 1029)) cluster = 1029;
    else if (number(clusterText, 1037, 1037)) cluster = 1037;
    else if (number(clusterText, 1066, 1066)) cluster = 1066;
    else return false;

    Fields value;
    if (!StatusJson(data.find("value")->second).parse(&value) || value.size() != 2 ||
        !value.count("type") || !value.count("value")) return false;
    MatterReading parsed;
    parsed.node = data.find("node")->second.substr(1, 16);
    parsed.endpoint = static_cast<unsigned short>(strtoul(data.find("endpoint")->second.c_str(), 0, 10));
    parsed.cluster = cluster;
    if (cluster == 6 || cluster == 69) {
        if (value["type"] != "\"bool\"" || (value["value"] != "true" && value["value"] != "false")) return false;
        parsed.known = true;
        parsed.booleanValue = value["value"] == "true";
    } else if (cluster == 8) {
        if (value["type"] != "\"u8\"") return false;
        parsed.known = value["value"] != "null";
        if (parsed.known) {
            if (!number(value["value"], 0, 254)) return false;
            parsed.numberValue = strtod(value["value"].c_str(), 0);
        }
    } else if (cluster == 91) {
        Fields quality;
        if (value["type"] != "\"air_quality\"" ||
            !StatusJson(value["value"]).parse(&quality) || quality.size() != 2 ||
            !quality.count("value") || !quality.count("quality")) return false;
        static const char* const qualities[] = {"\"good\"", "\"fair\"", "\"moderate\"",
            "\"poor\"", "\"very_poor\"", "\"extremely_poor\""};
        if (member(quality["value"], qualities, 6) && quality["quality"] == "\"measured\"") {
            parsed.known = true;
        } else if (quality["value"] == "\"unknown\"" && quality["quality"] == "\"unknown\"") {
            parsed.known = false;
        } else return false;
        parsed.textValue = quality["value"].substr(1, quality["value"].size() - 2);
    } else {
        if (value["type"] != "\"measurement\"" || !measurementValueValid(value["value"], cluster)) return false;
        Fields measurement;
        if (!StatusJson(value["value"]).parse(&measurement)) return false;
        parsed.known = measurement["value"] != "null";
        parsed.unit = measurement["unit"].substr(1, measurement["unit"].size() - 2);
        if (parsed.known) parsed.numberValue = strtod(measurement["value"].c_str(), 0);
    }
    *reading = parsed;
    return true;
}

bool eventValid(const std::string& text) {
    Fields fields, data;
    if (!StatusJson(text).parse(&fields) || fields.size() != 3 || fields["schema"] != "1" ||
        !fields.count("data") || !StatusJson(fields["data"]).parse(&data)) return false;
    if (fields["event"] == "\"thread\"") return snapshotValid(data, true);
    if (fields["event"] == "\"commissioning\"") return sessionValid(data);
    if (fields["event"] == "\"matter_report\"") {
        MatterReading reading;
        return matterReadingFromReport(data, &reading);
    }
    return fields["event"] == "\"ble_write\"" && data.size() == 4 && hex(data["session"], 16, 16) &&
        data["session"] != "\"0000000000000000\"" && number(data["generation"], 1, 0xffffffffU) &&
        number(data["write"], 1, 0xffffffffU) && hex(data["data"], 2, 488);
}

int64_t nowMilliseconds() {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

bool waitFor(int fd, bool writing, int64_t deadline) {
    while (true) {
        int64_t now = nowMilliseconds();
        if (now < 0 || fd < 0 || fd >= FD_SETSIZE) return false;
        int64_t remaining = deadline - now;
        if (remaining <= 0) return false;
        struct timeval timeout;
        timeout.tv_sec = static_cast<long>(remaining / 1000);
        timeout.tv_usec = static_cast<long>((remaining % 1000) * 1000);
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(fd, &ready);
        int result = select(fd + 1, writing ? 0 : &ready, writing ? &ready : 0, 0, &timeout);
        if (result > 0) return true;
        if (result == 0) return false;
        if (errno != EINTR) return false;
    }
}

bool writeAll(int fd, const unsigned char* data, unsigned int size, int64_t deadline) {
    unsigned int offset = 0;
    while (offset < size) {
        if (!waitFor(fd, true, deadline)) return false;
        ssize_t written = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (written > 0) offset += static_cast<unsigned int>(written);
        else if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        else return false;
    }
    return true;
}

bool readAll(int fd, unsigned char* data, unsigned int size, int64_t deadline) {
    unsigned int offset = 0;
    while (offset < size) {
        if (!waitFor(fd, false, deadline)) return false;
        ssize_t received = read(fd, data + offset, size - offset);
        if (received > 0) offset += static_cast<unsigned int>(received);
        else if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        else return false;
    }
    return true;
}
int connectPrivate(const std::string& socketPath, int64_t deadline, std::string* error) {
    struct stat pathInfo;
    if (socketPath.empty() || socketPath[0] != '/' || socketPath.size() >= sizeof(((struct sockaddr_un*)0)->sun_path) ||
        lstat(socketPath.c_str(), &pathInfo) != 0 || !S_ISSOCK(pathInfo.st_mode) ||
        pathInfo.st_uid != geteuid() || (pathInfo.st_mode & 0077) != 0) {
        *error = "Matter service socket unavailable";
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { *error = "Matter service unavailable"; return -1; }
    if (fd >= FD_SETSIZE || fcntl(fd, F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        close(fd); *error = "Matter service unavailable"; return -1;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        // Linux Unix sockets report EAGAIN when the listen backlog is full;
        // that is not an in-progress connection and must not be mistaken for one.
        if (errno != EINPROGRESS || !waitFor(fd, true, deadline)) {
            close(fd); *error = "Matter service unavailable"; return -1;
        }
        int connectionError = 0;
        socklen_t length = sizeof(connectionError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &connectionError, &length) != 0 || connectionError != 0) {
            close(fd); *error = "Matter service unavailable"; return -1;
        }
    }
    return fd;
}

}

MatterClient::MatterClient(const std::string& socketPath) : socketPath_(socketPath) {
}

bool MatterClient::status(bool thread, std::string* json, std::string* error) const {
    return call(thread ? MatterWire::ThreadStatus : MatterWire::MatterStatus, "", json, error);
}

bool MatterClient::observe(unsigned char channel, bool online, std::string* json, std::string* error) const {
    if ((online && (channel < 11 || channel > 26)) ||
        (!online && channel != 0 && (channel < 11 || channel > 26))) {
        json->clear(); *error = "Invalid coordinator observation"; return false;
    }
    std::ostringstream body;
    body << "{\"schema\":1,\"action\":\"observe\",\"zigbee_channel\":"
         << static_cast<unsigned int>(channel) << ",\"zigbee_online\":" << (online ? "true" : "false") << '}';
    return call(9, body.str(), json, error);
}

bool MatterClient::call(unsigned char opcode, const std::string& body, std::string* json, std::string* error) const {
    json->clear(); error->clear();
    const int64_t now = nowMilliseconds();
    if (now < 0) { *error = "Matter service clock unavailable"; return false; }
    const int64_t deadline = now + ioTimeoutSeconds * 1000;
    int fd = connectPrivate(socketPath_, deadline, error);
    if (fd < 0) return false;
    return exchange(fd, opcode, body, deadline, json, error);
}

bool MatterClient::requestStatus(int fd, bool thread, std::string* json, std::string* error) const {
    return exchange(fd, thread ? MatterWire::ThreadStatus : MatterWire::MatterStatus, "",
                    nowMilliseconds() + ioTimeoutSeconds * 1000, json, error);
}

bool MatterClient::exchange(int fd, unsigned char opcode, const std::string& body, int64_t deadline,
                            std::string* json, std::string* error) const {
    json->clear(); error->clear();
    MatterWire::Frame request;
    request.kind = MatterWire::Frame::Request;
    request.opcode = opcode;
    request.payload.assign(body.begin(), body.end());
    request.id = 1;
    MatterWire::Bytes encoded;
    if (!MatterWire::encode(request, &encoded) || !writeAll(fd, &encoded[0], encoded.size(), deadline)) {
        close(fd); *error = "Matter service request failed"; return false;
    }
    unsigned char header[10];
    if (!readAll(fd, header, sizeof(header), deadline)) {
        close(fd); *error = "Matter service response timed out"; return false;
    }
    unsigned int payloadSize = static_cast<unsigned int>(header[0]) |
                               (static_cast<unsigned int>(header[1]) << 8);
    if (payloadSize > MatterWire::MaxPayload) {
        close(fd); *error = "Matter service response invalid"; return false;
    }
    MatterWire::Bytes responseBytes(header, header + sizeof(header));
    responseBytes.resize(sizeof(header) + payloadSize);
    if (payloadSize != 0 && !readAll(fd, &responseBytes[sizeof(header)], payloadSize, deadline)) {
        close(fd); *error = "Matter service response timed out"; return false;
    }
    close(fd);

    MatterWire::Decoder decoder;
    if (!decoder.feed(responseBytes)) { *error = "Matter service response invalid"; return false; }
    MatterWire::Frame response;
    if (!decoder.pop(&response) || response.kind != MatterWire::Frame::Response ||
        response.id != request.id || response.opcode != request.opcode || response.payload.empty()) {
        *error = "Matter service response invalid";
        return false;
    }
    const std::string bodyJson(response.payload.begin(), response.payload.end());
    if (!responseValid(opcode, bodyJson)) { *error = "Matter service response invalid"; return false; }
    *json = bodyJson;
    return true;
}

MatterChannel::MatterChannel(const std::string& path) : socketPath_(path), fd_(-1), nextId_(1) {}
MatterChannel::~MatterChannel() { disconnect(); }
void MatterChannel::disconnect() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    decoder_ = MatterWire::Decoder();
    events_.clear();
}
bool MatterChannel::receive(int64_t deadline, bool wait) {
    if (fd_ < 0 || (wait && !waitFor(fd_, false, deadline))) return false;
    unsigned char buffer[MatterWire::MaxChunk];
    ssize_t count;
    do { count = recv(fd_, buffer, sizeof(buffer), MSG_DONTWAIT); } while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return !wait;
    return count > 0 && decoder_.feed(MatterWire::Bytes(buffer, buffer + count));
}
bool MatterChannel::event(const MatterWire::Frame& frame) {
    const std::string text(frame.payload.begin(), frame.payload.end());
    if (frame.kind != MatterWire::Frame::Event || frame.opcode != 12 || frame.id != 0 ||
        events_.size() >= 8 || !eventValid(text)) return false;
    events_.push_back(text);
    return true;
}
bool MatterChannel::popEvent(std::string* json) {
    json->clear();
    if (events_.empty()) return false;
    *json = events_.front(); events_.pop_front(); return true;
}
bool MatterChannel::poll(std::string* error) {
    error->clear();
    if (!receive(0, false)) { disconnect(); *error = "Matter service unavailable"; return false; }
    MatterWire::Frame frame;
    while (decoder_.pop(&frame)) {
        if (!event(frame)) { disconnect(); *error = "Matter service event invalid"; return false; }
    }
    return true;
}
bool MatterChannel::call(unsigned char opcode, const std::string& body, std::string* json, std::string* error) {
    json->clear(); error->clear();
    int64_t now = nowMilliseconds();
    if (now < 0 || nextId_ == 0 || opcode < 1 || opcode > 11 || body.size() > MatterWire::MaxPayload) {
        *error = "Matter service request invalid"; return false;
    }
    const int64_t deadline = now + 1000;
    if (fd_ < 0) fd_ = connectPrivate(socketPath_, deadline, error);
    if (fd_ < 0) return false;
    MatterWire::Frame request;
    request.kind = MatterWire::Frame::Request; request.opcode = opcode; request.id = nextId_++;
    request.payload.assign(body.begin(), body.end());
    MatterWire::Bytes bytes;
    bool valid = MatterWire::encode(request, &bytes) && writeAll(fd_, &bytes[0], bytes.size(), deadline);
    while (valid) {
        MatterWire::Frame frame;
        while (decoder_.pop(&frame)) {
            if (frame.kind == MatterWire::Frame::Event) { if (!event(frame)) { valid = false; break; } }
            else {
                const std::string text(frame.payload.begin(), frame.payload.end());
                if (frame.kind != MatterWire::Frame::Response || frame.id != request.id ||
                    frame.opcode != opcode || !responseValid(opcode, text)) { valid = false; break; }
                *json = text; return true;
            }
        }
        if (valid) valid = receive(deadline, true);
    }
    disconnect(); *error = "Matter service exchange failed"; return false;
}

bool MatterChannel::validRequest(unsigned char opcode, const std::string& body) {
    return requestValid(opcode, body);
}
bool MatterChannel::deviceSnapshotRequest(const std::string& body) {
    Fields fields;
    return StatusJson(body).parse(&fields) && fields.count("action") && fields["action"] == "\"snapshot\"";
}
bool MatterChannel::acceptedOperationJob(unsigned char opcode, const std::string& response, std::string* job) {
    job->clear();
    if ((opcode != 10 && opcode != 11) || !responseValid(opcode, response)) return false;
    Fields envelope, result;
    if (!StatusJson(response).parse(&envelope) || envelope["ok"] != "true" ||
        !StatusJson(envelope["result"]).parse(&result) || !acceptedOperationValid(result)) return false;
    *job = result["job"].substr(1, 16);
    return true;
}
bool MatterChannel::operationStatus(const std::string& response, const std::string& job,
                                    bool* terminal, bool* failed, std::string* error) {
    *terminal = false; *failed = false; error->clear();
    if (!responseValid(10, response) || job.size() != 16) return false;
    Fields envelope;
    if (!StatusJson(response).parse(&envelope)) return false;
    if (envelope["ok"] == "false") {
        Fields failure;
        if (!envelope.count("error")) return false;
        *terminal = true; *failed = true;
        *error = envelope["error"].substr(1, envelope["error"].size() - 2);
        return true;
    }
    Fields result;
    if (!StatusJson(envelope["result"]).parse(&result) || !operationalJobValid(result) ||
        result["job"] != '"' + job + '"') return false;
    if (result["state"] == "\"queued\"" || result["state"] == "\"running\"") return true;
    *terminal = true;
    if (result["state"] == "\"failed\"") {
        *failed = true;
        *error = result["error"].substr(1, result["error"].size() - 2);
    }
    return true;
}
std::string MatterChannel::operationPollRequest(const std::string& job) {
    if (!hex('"' + job + '"', 16, 16) || job == "0000000000000000") return std::string();
    return "{\"schema\":1,\"action\":\"job\",\"job\":\"" + job + "\"}";
}

bool MatterChannel::threadMutation(const std::string& action, const std::string& input, std::string* body) {
    body->clear();
    const bool importing = action == "import";
    const bool retrying = action == "retry";
    if (action != "create" && !importing && !retrying) return false;
    Fields fields;
    if (input.size() > MatterWire::MaxPayload || !StatusJson(input).parse(&fields) || fields["schema"] != "1" ||
        fields.size() != (importing ? 2U : 1U) || (importing && !hex(fields["dataset"], 2, 508))) return false;
    *body = importing ? "{\"schema\":1,\"action\":\"import\",\"dataset\":" + fields["dataset"] + '}' :
        retrying ? "{\"schema\":1,\"action\":\"retry\"}" :
                   "{\"schema\":1,\"action\":\"create\"}";
    return true;
}
bool MatterChannel::threadChannel(const std::string& input, unsigned char channel, std::string* body) {
    body->clear();
    if (channel < 11 || channel > 26 || !requestValid(9, input)) return false;
    Fields fields; if (!StatusJson(input).parse(&fields)) return false;
    if (fields["action"] == "\"retry\"") { *body = input; return true; }
    std::ostringstream out;
    out << "{\"schema\":1,\"action\":" << fields["action"] << ",\"zigbee_channel\":"
        << static_cast<unsigned int>(channel);
    if (fields["action"] == "\"import\"") out << ",\"dataset\":" << fields["dataset"];
    out << '}'; *body = out.str(); return true;
}
bool MatterChannel::deviceReadRequest(const std::string& input, std::string* body) {
    body->clear();
    Fields fields;
    if (input.size() > MatterWire::MaxPayload || !StatusJson(input).parse(&fields) || fields.size() != 4 ||
        !hex(fields["node"], 16, 16) || fields["node"] <= "\"0000000000000001\"" ||
        !number(fields["endpoint"], 0, 65534) ||
        (fields["cluster"] != "6" && fields["cluster"] != "8" && fields["cluster"] != "69" && fields["cluster"] != "91" &&
         fields["cluster"] != "1026" && fields["cluster"] != "1029" &&
         fields["cluster"] != "1037" && fields["cluster"] != "1066") || fields["attribute"] != "0") return false;
    *body = "{\"schema\":1,\"action\":\"read\",\"node\":" + fields["node"] +
        ",\"endpoint\":" + fields["endpoint"] + ",\"cluster\":" + fields["cluster"] + ",\"attribute\":0}";
    return requestValid(10, *body);
}
bool MatterChannel::deviceCommandRequest(const std::string& input, std::string* body) {
    body->clear();
    Fields fields;
    static const char* const commands[] = {"\"on\"", "\"off\"", "\"toggle\""};
    if (input.size() > MatterWire::MaxPayload || !StatusJson(input).parse(&fields) ||
        !hex(fields["node"], 16, 16) || fields["node"] <= "\"0000000000000001\"" ||
        !number(fields["endpoint"], 0, 65534)) return false;
    const bool level = fields["command"] == "\"move_to_level\"";
    if ((level && (fields.size() != 5 || !number(fields["level"], 0, 254) ||
                   !number(fields["transition_deciseconds"], 0, 65535))) ||
        (!level && (fields.size() != 3 || !member(fields["command"], commands, 3)))) return false;
    *body = "{\"schema\":1,\"command\":" + fields["command"] + ",\"node\":" + fields["node"] +
        ",\"endpoint\":" + fields["endpoint"];
    if (level) *body += ",\"level\":" + fields["level"] + ",\"transition_deciseconds\":" + fields["transition_deciseconds"];
    *body += '}';
    return requestValid(11, *body);
}
bool MatterChannel::devicePage(const std::string& response, std::string* revision, long* nextCursor,
                              std::vector<MatterEndpoint>* endpoints) {
    revision->clear(); *nextCursor = -1; endpoints->clear();
    if (!responseValid(10, response)) return false;
    Fields envelope, result;
    if (!StatusJson(response).parse(&envelope) || !StatusJson(envelope["result"]).parse(&result) ||
        !devicePageValid(result)) return false;
    *revision = result["revision"].substr(1, result["revision"].size() - 2);
    if (result["next_cursor"] != "null") {
        if (!number(result["next_cursor"], 1, 2047)) return false;
        *nextCursor = strtol(result["next_cursor"].c_str(), 0, 10);
    }
    std::vector<std::string> rows;
    if (!StatusJson(result["devices"]).parseArray(&rows)) return false;
    for (unsigned long i = 0; i < rows.size(); ++i) {
        Fields row;
        if (!StatusJson(rows[i]).parse(&row) || row["endpoint"] == "null") continue;
        MatterEndpoint endpoint;
        endpoint.id = row["id"].substr(1, row["id"].size() - 2);
        endpoint.node = row["node"].substr(1, row["node"].size() - 2);
        endpoint.state = row["state"].substr(1, row["state"].size() - 2);
        endpoint.endpoint = static_cast<unsigned short>(strtoul(row["endpoint"].c_str(), 0, 10));
        endpoint.online = row["online"] == "true";
        std::vector<std::string> clusters;
        if (!StatusJson(row["servers"]).parseArray(&clusters)) return false;
        for (unsigned long cluster = 0; cluster < clusters.size(); ++cluster)
            endpoint.servers.push_back(static_cast<unsigned int>(strtoul(clusters[cluster].c_str(), 0, 10)));
        endpoints->push_back(endpoint);
    }
    return true;
}

bool MatterChannel::deviceResult(const std::string& response, const std::string& job, MatterReading* reading) {
    if (!reading || !responseValid(10, response)) return false;
    Fields envelope, status, result, value;
    if (!StatusJson(response).parse(&envelope) || envelope["ok"] != "true" ||
        !StatusJson(envelope["result"]).parse(&status) || !operationalJobValid(status) ||
        status["state"] != "\"complete\"" || status["job"] != '"' + job + '"' ||
        !StatusJson(status["result"]).parse(&result) || !StatusJson(result["value"]).parse(&value)) return false;
    MatterReading parsed;
    parsed.node = result["node"].substr(1, result["node"].size() - 2);
    parsed.endpoint = static_cast<unsigned short>(strtoul(result["endpoint"].c_str(), 0, 10));
    parsed.cluster = static_cast<unsigned int>(strtoul(result["cluster"].c_str(), 0, 10));
    if (parsed.cluster == 6 || parsed.cluster == 69) {
        parsed.known = true;
        parsed.booleanValue = value["value"] == "true";
    } else if (parsed.cluster == 8) {
        parsed.known = value["value"] != "null";
        if (parsed.known) parsed.numberValue = strtod(value["value"].c_str(), 0);
    } else if (parsed.cluster == 91) {
        Fields air;
        if (!StatusJson(value["value"]).parse(&air)) return false;
        parsed.known = air["quality"] == "\"measured\"";
        parsed.textValue = air["value"].substr(1, air["value"].size() - 2);
    } else {
        Fields measurement;
        if (!StatusJson(value["value"]).parse(&measurement)) return false;
        parsed.known = measurement["quality"] == "\"measured\"" && measurement["value"] != "null";
        parsed.unit = measurement["unit"].substr(1, measurement["unit"].size() - 2);
        if (parsed.known) parsed.numberValue = strtod(measurement["value"].c_str(), 0);
    }
    *reading = parsed;
    return true;
}

bool MatterChannel::deviceReportEvent(const std::string& input, MatterReading* reading) {
    if (!reading || !eventValid(input)) return false;
    Fields envelope, data;
    if (!StatusJson(input).parse(&envelope) ||
        !StatusJson(envelope["data"]).parse(&data)) return false;
    return matterReadingFromReport(data, reading);
}

bool MatterChannel::relayRequest(const std::string& input, uint32_t* id, unsigned char* opcode,
                                std::string* session, std::string* body) {
    *id = 0; *opcode = 0; session->clear(); body->clear();
    Fields fields, request;
    if (input.size() > MatterWire::MaxPayload || !StatusJson(input).parse(&fields) || fields.size() != 4 ||
        fields["schema"] != "1" || !number(fields["id"], 1, 0xffffffffU) || !number(fields["opcode"], 4, 8) ||
        !fields.count("body") || !StatusJson(fields["body"]).parse(&request)) return false;
    unsigned int operation = static_cast<unsigned int>(fields["opcode"][0] - '0');
    if (!requestValid(operation, fields["body"])) return false;
    uint64_t correlation = 0;
    for (unsigned long i = 0; i < fields["id"].size(); ++i) correlation = correlation * 10 + fields["id"][i] - '0';
    *id = static_cast<uint32_t>(correlation); *opcode = static_cast<unsigned char>(operation);
    *session = request["session"].substr(1, 16);
    *body = operation == 4 ? "{\"schema\":1,\"session\":\"" + *session + "\",\"subscribe\":true}" : fields["body"];
    return true;
}
uint32_t MatterChannel::relayGeneration(const std::string& response) {
    Fields fields, result;
    if (!StatusJson(response).parse(&fields) || fields["ok"] != "true" ||
        !StatusJson(fields["result"]).parse(&result) || !number(result["generation"], 1, 0xffffffffU)) return 0;
    uint64_t value = 0;
    for (unsigned long i = 0; i < result["generation"].size(); ++i) value = value * 10 + result["generation"][i] - '0';
    return static_cast<uint32_t>(value);
}
bool MatterChannel::relayEvent(const std::string& input, const std::string& session, uint32_t generation) {
    Fields fields, data;
    if (!eventValid(input) || !StatusJson(input).parse(&fields) ||
        !StatusJson(fields["data"]).parse(&data) || data["session"] != '"' + session + '"') return false;
    if (fields["event"] == "\"commissioning\"") return true;
    std::ostringstream value; value << generation;
    return fields["event"] == "\"ble_write\"" && generation != 0 && data["generation"] == value.str();
}

std::string MatterChannel::relayResponseSession(const std::string& response) {
    Fields fields, result;
    if (!StatusJson(response).parse(&fields) || fields["ok"] != "true" ||
        !StatusJson(fields["result"]).parse(&result) || !hex(result["session"], 16, 16)) return "";
    return result["session"].substr(1, 16);
}
