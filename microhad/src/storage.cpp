#include "storage.h"
#include "gateway.h"
#include "json_util.h"
#include "protobuf_value.h"
#define PB_C99_STATIC_ASSERT
#include "../../third-party/nanopb/pb_decode.h"
#include "../../third-party/nanopb/pb_encode.h"
#include "../../protocol/generated/device-cache.pb.h"
#include <cmath>
#include <set>
#include <stdint.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

NetworkState::NetworkState() : formed(false), panId(0), channel(0) {
}

namespace {
std::string hexEncode(const std::vector<unsigned char>& input) {
    const char* digits = "0123456789abcdef";
    std::string output;
    unsigned long i;
    for (i = 0; i < input.size(); ++i) {
        output += digits[input[i] >> 4];
        output += digits[input[i] & 0x0f];
    }
    return output;
}

bool hexDecode(const std::string& input, std::vector<unsigned char>* output) {
    unsigned long i;
    if ((input.size() & 1) != 0) return false;
    output->clear();
    for (i = 0; i < input.size(); i += 2) {
        int high = input[i] >= '0' && input[i] <= '9' ? input[i] - '0' :
                   input[i] >= 'a' && input[i] <= 'f' ? input[i] - 'a' + 10 : -1;
        int low = input[i + 1] >= '0' && input[i + 1] <= '9' ? input[i + 1] - '0' :
                  input[i + 1] >= 'a' && input[i + 1] <= 'f' ? input[i + 1] - 'a' + 10 : -1;
        if (high < 0 || low < 0) return false;
        output->push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return true;
}

bool decimalField(const std::string& text, unsigned long maximum, unsigned long* value) {
    unsigned long parsed = 0;
    if (text.empty()) return false;
    for (unsigned long i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9' || parsed > (maximum - static_cast<unsigned long>(text[i] - '0')) / 10)
            return false;
        parsed = parsed * 10 + static_cast<unsigned long>(text[i] - '0');
    }
    *value = parsed;
    return true;
}

bool brightnessField(const std::string& text, int* value) {
    unsigned long parsed;
    if (text == "-1") { *value = -1; return true; }
    if (!decimalField(text, 100, &parsed)) return false;
    *value = static_cast<int>(parsed);
    return true;
}

std::string encodeEndpointDescriptors(const std::vector<EndpointDescriptor>& endpoints) {
    std::vector<unsigned char> bytes;
    if (endpoints.size() > 255) return "";
    bytes.push_back(1); /* encoding version */
    bytes.push_back(static_cast<unsigned char>(endpoints.size()));
    for (unsigned long i = 0; i < endpoints.size(); ++i) {
        const EndpointDescriptor& endpoint = endpoints[i];
        if (endpoint.inputClusters.size() > 255 || endpoint.outputClusters.size() > 255) return "";
        bytes.push_back(endpoint.endpoint);
        bytes.push_back(static_cast<unsigned char>(endpoint.profileId));
        bytes.push_back(static_cast<unsigned char>(endpoint.profileId >> 8));
        bytes.push_back(static_cast<unsigned char>(endpoint.deviceId));
        bytes.push_back(static_cast<unsigned char>(endpoint.deviceId >> 8));
        bytes.push_back(endpoint.deviceVersion);
        bytes.push_back(static_cast<unsigned char>(endpoint.inputClusters.size()));
        for (unsigned long j = 0; j < endpoint.inputClusters.size(); ++j) {
            bytes.push_back(static_cast<unsigned char>(endpoint.inputClusters[j]));
            bytes.push_back(static_cast<unsigned char>(endpoint.inputClusters[j] >> 8));
        }
        bytes.push_back(static_cast<unsigned char>(endpoint.outputClusters.size()));
        for (unsigned long j = 0; j < endpoint.outputClusters.size(); ++j) {
            bytes.push_back(static_cast<unsigned char>(endpoint.outputClusters[j]));
            bytes.push_back(static_cast<unsigned char>(endpoint.outputClusters[j] >> 8));
        }
    }
    return hexEncode(bytes);
}

bool decodeEndpointDescriptors(const std::string& encoded, std::vector<EndpointDescriptor>* endpoints) {
    std::vector<unsigned char> bytes;
    unsigned long offset = 0;
    if (!hexDecode(encoded, &bytes) || bytes.size() < 2 || bytes[0] != 1) return false;
    unsigned long endpointCount = bytes[1];
    offset = 2;
    endpoints->clear();
    for (unsigned long i = 0; i < endpointCount; ++i) {
        EndpointDescriptor endpoint;
        if (offset + 7 > bytes.size()) return false;
        endpoint.endpoint = bytes[offset++];
        endpoint.profileId = static_cast<unsigned short>(bytes[offset]) | (static_cast<unsigned short>(bytes[offset + 1]) << 8); offset += 2;
        endpoint.deviceId = static_cast<unsigned short>(bytes[offset]) | (static_cast<unsigned short>(bytes[offset + 1]) << 8); offset += 2;
        endpoint.deviceVersion = bytes[offset++];
        unsigned long count = bytes[offset++];
        if (offset + count * 2 + 1 > bytes.size()) return false;
        for (unsigned long j = 0; j < count; ++j, offset += 2)
            endpoint.inputClusters.push_back(static_cast<unsigned short>(bytes[offset]) | (static_cast<unsigned short>(bytes[offset + 1]) << 8));
        count = bytes[offset++];
        if (offset + count * 2 > bytes.size()) return false;
        for (unsigned long j = 0; j < count; ++j, offset += 2)
            endpoint.outputClusters.push_back(static_cast<unsigned short>(bytes[offset]) | (static_cast<unsigned short>(bytes[offset + 1]) << 8));
        endpoints->push_back(endpoint);
    }
    return offset == bytes.size();
}

std::string encodeMatterServers(const std::vector<unsigned int>& servers) {
    std::vector<unsigned char> bytes;
    if (servers.size() > 64) return "";
    for (unsigned long i = 0; i < servers.size(); ++i) {
        bytes.push_back(static_cast<unsigned char>(servers[i] >> 24));
        bytes.push_back(static_cast<unsigned char>(servers[i] >> 16));
        bytes.push_back(static_cast<unsigned char>(servers[i] >> 8));
        bytes.push_back(static_cast<unsigned char>(servers[i]));
    }
    return hexEncode(bytes);
}

bool decodeMatterServers(const std::string& encoded, std::vector<unsigned int>* servers) {
    std::vector<unsigned char> bytes;
    if (!hexDecode(encoded, &bytes) || bytes.size() % 4 != 0 || bytes.size() > 256) return false;
    servers->clear();
    for (unsigned long i = 0; i < bytes.size(); i += 4) {
        unsigned int value = (static_cast<unsigned int>(bytes[i]) << 24) |
                             (static_cast<unsigned int>(bytes[i + 1]) << 16) |
                             (static_cast<unsigned int>(bytes[i + 2]) << 8) |
                             static_cast<unsigned int>(bytes[i + 3]);
        if (std::find(servers->begin(), servers->end(), value) != servers->end()) return false;
        servers->push_back(value);
    }
    return true;
}

bool validOptionalIntegerField(const std::string& line, const char* field, long minimum, long maximum) {
    std::string key = std::string("\"") + field + "\":";
    std::string::size_type at = line.find(key);
    if (at == std::string::npos) return true;
    if (line.find(key, at + key.size()) != std::string::npos) return false;
    at += key.size();
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
    if (at == line.size() || !((line[at] >= '0' && line[at] <= '9') ||
        (line[at] == '-' && at + 1 < line.size() && line[at + 1] >= '0' && line[at + 1] <= '9'))) return false;
    errno = 0;
    char* end = 0;
    long value = strtol(line.c_str() + at, &end, 10);
    if (errno != 0 || end == line.c_str() + at || value < minimum || value > maximum) return false;
    while (*end == ' ' || *end == '\t') ++end;
    return *end == ',' || *end == '}';
}

bool validOptionalDecimalField(const std::string& line, const char* field, double minimum, double maximum) {
    std::string key = std::string("\"") + field + "\":";
    std::string::size_type at = line.find(key);
    if (at == std::string::npos) return true;
    if (line.find(key, at + key.size()) != std::string::npos) return false;
    at += key.size();
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
    errno = 0;
    char* end = 0;
    double value = strtod(line.c_str() + at, &end);
    if (errno != 0 || end == line.c_str() + at || value != value || value < minimum || value > maximum) return false;
    while (*end == ' ' || *end == '\t') ++end;
    return *end == ',' || *end == '}';
}

const char* numericField(const std::string& line, const char* field) {
    const std::string key = std::string("\"") + field + "\":";
    const std::string::size_type at = line.find(key);
    return at == std::string::npos ? 0 : line.c_str() + at + key.size();
}

}

bool ensureDirectory(const std::string& directory, std::string* error) {
    if (mkdir(directory.c_str(), 0700) == 0) return true;
    if (errno == EEXIST) {
        struct stat details;
        if (stat(directory.c_str(), &details) == 0 && S_ISDIR(details.st_mode)) return true;
    }
    *error = std::string("cannot create runtime directory: ") + strerror(errno);
    return false;
}


namespace {
pthread_mutex_t dashboardMutex = PTHREAD_MUTEX_INITIALIZER;
bool writeDashboard(const std::string& filename, const std::string& value) {
    const std::string temporary = filename + ".tmp";
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    unsigned long written = 0;
    while (written < value.size()) {
        ssize_t size = write(fd, value.data() + written, value.size() - written);
        if (size <= 0) { close(fd); unlink(temporary.c_str()); return false; }
        written += size;
    }
    bool synced = fsync(fd) == 0;
    bool closed = close(fd) == 0;
    bool ok = synced && closed && rename(temporary.c_str(), filename.c_str()) == 0;
    if (!ok) unlink(temporary.c_str());
    return ok;
}
// Match the top-level property, including escaped keys, without confusing a
// metadata string containing "widgets" with the actual widget array.
std::string::size_type widgetArrayStart(const std::string& content) {
    unsigned int depth = 0;
    std::string::size_type result = std::string::npos;
    for (unsigned long at = 0; at < content.size(); ++at) {
        const char c = content[at];
        if (c == '{' || c == '[') { ++depth; continue; }
        if (c == '}' || c == ']') { if (depth == 0) return std::string::npos; --depth; continue; }
        if (c != '"') continue;
        const unsigned long start = at++;
        bool escaped = false;
        while (at < content.size()) {
            if (escaped) escaped = false;
            else if (content[at] == '\\') escaped = true;
            else if (content[at] == '"') break;
            ++at;
        }
        if (at == content.size()) return std::string::npos;
        unsigned long next = at + 1;
        while (next < content.size() && isspace(static_cast<unsigned char>(content[next]))) ++next;
        if (depth != 1 || next == content.size() || content[next] != ':') continue;
        std::string key;
        if (!jsonStringField("{\"key\":" + content.substr(start, at - start + 1) + "}", "key", &key)) return std::string::npos;
        if (key != "widgets") continue;
        ++next;
        while (next < content.size() && isspace(static_cast<unsigned char>(content[next]))) ++next;
        if (result != std::string::npos || next == content.size() || content[next] != '[') return std::string::npos;
        result = next;
    }
    return depth == 0 ? result : std::string::npos;
}
bool pruneDashboard(const std::string& filename, const std::vector<std::string>& ids, std::string* error) {
    std::string content;
    if (access(filename.c_str(), F_OK) != 0 && errno == ENOENT) return true;
    if (!readTextFile(filename, &content) || content.size() > 12000) { *error = "Cannot read dashboard configuration"; return false; }
    std::string::size_type begin = widgetArrayStart(content);
    if (begin == std::string::npos) { *error = "Invalid dashboard configuration"; return false; }
    std::string kept;
    unsigned long at = begin + 1;
    while (at < content.size()) {
        while (at < content.size() && (isspace(static_cast<unsigned char>(content[at])) || content[at] == ',')) ++at;
        if (at < content.size() && content[at] == ']') break;
        unsigned long start = at;
        int depth = 0; bool quoted = false, escaped = false;
        if (at >= content.size() || content[at] != '{') { *error = "Invalid dashboard widget"; return false; }
        for (; at < content.size(); ++at) {
            char c = content[at];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (--depth == 0) { ++at; break; } }
        }
        if (depth != 0 || quoted) { *error = "Invalid dashboard widget"; return false; }
        std::string widget = content.substr(start, at - start), target;
        jsonStringField(widget, "target", &target);
        if (std::find(ids.begin(), ids.end(), target) == ids.end()) { if (!kept.empty()) kept += ','; kept += widget; }
    }
    if (at >= content.size()) { *error = "Invalid dashboard configuration"; return false; }
    std::string result = content.substr(0, begin + 1) + kept + content.substr(at);
    if (result == content) return true;
    if (!writeDashboard(filename, result)) { *error = "Cannot save dashboard configuration"; return false; }
    return true;
}
}
bool saveDashboardConfiguration(const std::string& filename, const std::string& value) {
    pthread_mutex_lock(&dashboardMutex); bool ok = writeDashboard(filename, value); pthread_mutex_unlock(&dashboardMutex); return ok;
}
bool pruneDashboardDevices(const std::string& filename, const std::vector<std::string>& addresses, std::string* error) {
    pthread_mutex_lock(&dashboardMutex); bool ok = pruneDashboard(filename, addresses, error); pthread_mutex_unlock(&dashboardMutex); return ok;
}

bool saveDevices(const std::string& filename, const std::vector<Device>& devices, std::string* error) {
    std::string temporary = filename + ".tmp";
    std::ofstream output(temporary.c_str());
    unsigned long i;
    if (!output) {
        *error = "cannot open device state for writing";
        return false;
    }
    output << "{\n  \"version\": 2,\n  \"devices\": [\n";
    for (i = 0; i < devices.size(); ++i) {
        const Device& device = devices[i];
        output << "    {\"ieee_address\":\"" << jsonEscape(device.ieeeAddress)
               << "\",\"source\":\"" << jsonEscape(device.source)
               << "\",\"external_id\":\"" << jsonEscape(device.external.read().externalId)
               << "\",\"component\":\"" << jsonEscape(device.external.read().component)
               << "\",\"network_address\":" << device.zigbee.read().networkAddress << ",\"name\":\"" << jsonEscape(device.name) << "\",\"kind\":\""
               << jsonEscape(device.kind) << "\",\"contact_open\":" << (device.contact.read().open ? "true" : "false")
               << ",\"has_contact\":" << (device.contact.present() ? "true" : "false")
               << ",\"contact_known\":" << (device.contact.read().known ? "true" : "false")
               << ",\"has_water_leak\":" << (device.waterLeak.present() ? "true" : "false")
               << ",\"water_leak_known\":" << (device.waterLeak.read().known ? "true" : "false")
               << ",\"water_detected\":" << (device.waterLeak.read().detected ? "true" : "false")
               << ",\"temperature_celsius\":" << device.temperature.read().celsius << ",\"has_temperature\":"
               << (device.temperature.present() ? "true" : "false") << ",\"humidity_percent\":" << device.humidity.read().percent
               << ",\"temperature_known\":" << (device.temperature.read().known ? "true" : "false")
               << ",\"has_humidity\":" << (device.humidity.present() ? "true" : "false") << ",\"occupied\":"
               << (device.occupancy.read().occupied ? "true" : "false") << ",\"has_occupancy\":"
               << (device.occupancy.present() ? "true" : "false") << ",\"battery_percent\":" << device.battery.read().percent
               << ",\"has_battery\":" << (device.battery.present() ? "true" : "false") << ",\"link_quality\":"
               << device.zigbee.read().linkQuality << ",\"has_wifi_signal\":" << (device.wifiSignal.present() ? "true" : "false")
               << ",\"wifi_signal_percent\":" << device.wifiSignal.read().percent << ",\"last_seen\":" << device.lastSeen << ",\"ias_zone_id\":" << device.zigbee.read().iasZoneId
               << ",\"has_light\":" << (device.light.present() ? "true" : "false") << ",\"light_on\":"
               << (device.light.read().on ? "true" : "false") << ",\"has_brightness\":"
               << (device.brightness.present() ? "true" : "false") << ",\"brightness_percent\":" << device.brightness.read().percent
               << ",\"has_remote\":" << (device.remote.present() ? "true" : "false")
               << ",\"has_actor\":" << (device.actor.present() ? "true" : "false")
               << ",\"actor_known\":" << (device.actor.read().known ? "true" : "false")
               << ",\"actor_on\":" << (device.actor.read().on ? "true" : "false")
               << ",\"has_power\":" << (device.power.present() ? "true" : "false")
               << ",\"power_known\":" << (device.power.read().known ? "true" : "false")
               << ",\"power_watts\":" << device.power.read().watts
               << ",\"has_low_temperature_alert\":"
               << (device.hasLowTemperatureAlert ? "true" : "false") << ",\"low_temperature_alert\":"
               << device.lowTemperatureAlert << ",\"has_high_temperature_alert\":"
               << (device.hasHighTemperatureAlert ? "true" : "false") << ",\"high_temperature_alert\":"
               << device.highTemperatureAlert << ",\"contact_initial_alert_minutes\":"
               << device.contactInitialAlertMinutes << ",\"contact_reminder_alert_minutes\":"
               << device.contactReminderAlertMinutes << ",\"contact_trigger_mode\":"
               << (device.contactTriggerMode ? "true" : "false") << ",\"contact_trigger_cooldown_seconds\":"
               << device.contactTriggerCooldownSeconds << ",\"sensor_refresh_interval_minutes\":"
               << device.sensorRefreshIntervalMinutes
               << ",\"thermostat_source_id\":\"" << jsonEscape(device.thermostatSourceId) << "\""
               << ",\"thermostat_stale_minutes\":" << device.thermostatStaleMinutes
               << ",\"humidity_known\":" << (device.humidity.read().known ? "true" : "false")
               << ",\"has_air_quality\":" << (device.airQuality.present() ? "true" : "false")
               << ",\"air_quality_known\":" << (device.airQuality.read().known ? "true" : "false")
               << ",\"air_quality\":\"" << jsonEscape(device.airQuality.read().value)
               << "\",\"has_co2\":" << (device.co2.present() ? "true" : "false")
               << ",\"co2_known\":" << (device.co2.read().known ? "true" : "false")
               << ",\"co2_ppm\":" << device.co2.read().ppm << ",\"co2_unit\":\"" << jsonEscape(device.co2.read().unit)
               << "\",\"has_pm25\":" << (device.pm25.present() ? "true" : "false")
               << ",\"pm25_known\":" << (device.pm25.read().known ? "true" : "false")
               << ",\"pm25_micrograms_m3\":" << device.pm25.read().microgramsM3
               << ",\"pm25_unit\":\"" << jsonEscape(device.pm25.read().unit)
               << "\",\"light_known\":" << (device.light.read().known ? "true" : "false")
               << ",\"brightness_known\":" << (device.brightness.read().known ? "true" : "false");
        for (std::map<std::string, DeviceValueAlert>::const_iterator alert = device.valueAlerts.begin(); alert != device.valueAlerts.end(); ++alert) {
            output << ",\"alert_" << alert->first << "_has_low\":" << (alert->second.hasLow ? "true" : "false")
                   << ",\"alert_" << alert->first << "_low\":" << alert->second.low
                   << ",\"alert_" << alert->first << "_has_high\":" << (alert->second.hasHigh ? "true" : "false")
                   << ",\"alert_" << alert->first << "_high\":" << alert->second.high
                   << ",\"alert_" << alert->first << "_hysteresis\":" << alert->second.hysteresis
                   << ",\"alert_" << alert->first << "_unit\":\"" << jsonEscape(alert->second.unit) << '"';
        }
        output << ",\"initial_interview_complete\":" << (device.zigbee.read().initialInterviewComplete ? "true" : "false");
        output << ",\"manufacturer_name\":\""
               << jsonEscape(device.manufacturerName) << "\",\"model_id\":\"" << jsonEscape(device.modelId)
               << "\",\"software_build_id\":\"" << jsonEscape(device.softwareBuildId)
               << "\",\"configured_signature\":\"" << jsonEscape(device.zigbee.read().configuredSignature)
               << "\",\"endpoint_descriptors\":\"" << encodeEndpointDescriptors(device.zigbee.read().endpoints)
               << "\",\"node_descriptor\":\"" << hexEncode(device.zigbee.read().nodeDescriptor) << '"';
        if (device.source == "matter") {
            output << ",\"matter_state\":\"" << jsonEscape(device.matter.read().state)
                   << "\",\"matter_metadata_state\":\"" << jsonEscape(device.matter.read().metadataState) << '"'
                   << ",\"matter_node\":\"" << jsonEscape(device.matter.read().node)
                   << "\",\"matter_endpoint\":" << device.matter.read().endpoint
                   << ",\"matter_servers\":\"" << encodeMatterServers(device.matter.read().servers)
                   << "\",\"matter_clients\":\"" << encodeMatterServers(device.matter.read().clients)
                   << "\",\"matter_device_types\":\"" << encodeMatterServers(device.matter.read().deviceTypes) << '"';
        }
        output << '}';
        output << (i + 1 == devices.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    output.close();
    if (!output || rename(temporary.c_str(), filename.c_str()) != 0) {
        unlink(temporary.c_str());
        *error = "cannot atomically save device state";
        return false;
    }
    return true;
}

bool loadDevices(const std::string& filename, std::vector<Device>* devices, std::string* error) {
    std::ifstream input(filename.c_str());
    std::string line;
    std::vector<Device> parsedDevices;
    if (!input) {
        devices->clear();
        return true;
    }
    /* The state writer emits one device per line. Unknown fields are ignored so
       newer state files remain readable by an older daemon. */
    while (std::getline(input, line)) {
        std::string ieeeAddress;
        if (!line.empty() && line[line.size() - 1] == ',') line.erase(line.size() - 1);
        if (jsonStringField(line, "ieee_address", &ieeeAddress)) {
            long networkAddress = 0;
            const char* addressValue = numericField(line, "network_address");
            if (addressValue != 0) {
                if (!validOptionalIntegerField(line, "network_address", 0, 0xffff)) continue;
                networkAddress = strtol(addressValue, 0, 10);
            }
            if (ieeeAddress.empty() ||
                !validOptionalDecimalField(line, "temperature_celsius", -100, 200) ||
                !validOptionalDecimalField(line, "humidity_percent", 0, 100) ||
                !validOptionalDecimalField(line, "co2_ppm", 0, 3.402823466e38) ||
                !validOptionalDecimalField(line, "pm25_micrograms_m3", 0, 3.402823466e38) ||
                !validOptionalDecimalField(line, "power_watts", 0, 6553.5) ||
                !validOptionalDecimalField(line, "low_temperature_alert", -100, 200) ||
                !validOptionalDecimalField(line, "high_temperature_alert", -100, 200) ||
                !validOptionalIntegerField(line, "battery_percent", -1, 100) ||
                !validOptionalIntegerField(line, "link_quality", 0, 255) ||
                !validOptionalIntegerField(line, "wifi_signal_percent", 0, 100) ||
                !validOptionalIntegerField(line, "brightness_percent", -1, 100) ||
                !validOptionalIntegerField(line, "last_seen", 0, LONG_MAX) ||
                !validOptionalIntegerField(line, "matter_endpoint", 0, 65534) ||
                !validOptionalIntegerField(line, "ias_zone_id", -1, 254) ||
                !validOptionalIntegerField(line, "contact_initial_alert_minutes", 0, LONG_MAX) ||
                !validOptionalIntegerField(line, "contact_reminder_alert_minutes", 0, LONG_MAX) ||
                !validOptionalIntegerField(line, "contact_trigger_cooldown_seconds", 1, LONG_MAX) ||
                !validOptionalIntegerField(line, "sensor_refresh_interval_minutes", 0, LONG_MAX)) continue;
            bool duplicate = false;
            for (unsigned long previous = 0; previous < parsedDevices.size(); ++previous) {
                if (parsedDevices[previous].ieeeAddress == ieeeAddress ||
                    (networkAddress != 0xffff && parsedDevices[previous].zigbee.read().networkAddress == networkAddress)) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            Device device;
            device.ieeeAddress = ieeeAddress;
            jsonStringField(line, "source", &device.source);
            if (device.source != "zigbee" && device.source != "shelly" && device.source != "fritz_box" &&
                device.source != "matter") continue;
            jsonStringField(line, "external_id", &device.external.ensure().externalId);
            jsonStringField(line, "component", &device.external.ensure().component);
            if (addressValue != 0) device.zigbee.ensure().networkAddress = static_cast<unsigned short>(networkAddress);
            const char* fields[] = {"name", "kind", "manufacturer_name", "model_id", "software_build_id"};
            std::string* destinations[] = {&device.name, &device.kind, &device.manufacturerName, &device.modelId, &device.softwareBuildId};
            int index;
            for (index = 0; index < 5; ++index) {
                jsonStringField(line, fields[index], destinations[index]);
            }
            jsonStringField(line, "configured_signature", &device.zigbee.ensure().configuredSignature);
            jsonStringField(line, "thermostat_source_id", &device.thermostatSourceId);
            long staleMinutes = 90;
            if (jsonLongField(line, "thermostat_stale_minutes", &staleMinutes) && staleMinutes >= 5 && staleMinutes <= 1440)
                device.thermostatStaleMinutes = static_cast<unsigned long>(staleMinutes);
            device.contact.setPresent(line.find("\"has_contact\":true") != std::string::npos);
            device.waterLeak.setPresent(line.find("\"has_water_leak\":true") != std::string::npos);
            device.temperature.setPresent(line.find("\"has_temperature\":true") != std::string::npos);
            device.humidity.setPresent(line.find("\"has_humidity\":true") != std::string::npos);
            device.airQuality.setPresent(line.find("\"has_air_quality\":true") != std::string::npos);
            device.co2.setPresent(line.find("\"has_co2\":true") != std::string::npos);
            device.pm25.setPresent(line.find("\"has_pm25\":true") != std::string::npos);
            device.occupancy.setPresent(line.find("\"has_occupancy\":true") != std::string::npos);
            device.battery.setPresent(line.find("\"has_battery\":true") != std::string::npos);
            device.wifiSignal.setPresent(line.find("\"has_wifi_signal\":true") != std::string::npos);
            device.light.setPresent(line.find("\"has_light\":true") != std::string::npos);
            device.brightness.setPresent(line.find("\"has_brightness\":true") != std::string::npos);
            device.remote.setPresent(line.find("\"has_remote\":true") != std::string::npos);
            device.actor.setPresent(line.find("\"has_actor\":true") != std::string::npos);
            device.power.setPresent(line.find("\"has_power\":true") != std::string::npos);
            if(device.contact.present()) device.contact.ensure().open = line.find("\"contact_open\":true") != std::string::npos;
            if(device.contact.present()) device.contact.ensure().known = line.find("\"contact_known\":true") != std::string::npos;
            if(device.waterLeak.present()) device.waterLeak.ensure().known = line.find("\"water_leak_known\":true") != std::string::npos;
            if(device.waterLeak.present()) device.waterLeak.ensure().detected = line.find("\"water_detected\":true") != std::string::npos;
            if(device.temperature.present()) device.temperature.ensure().known = line.find("\"temperature_known\":true") != std::string::npos;
            if(device.humidity.present()) device.humidity.ensure().known = line.find("\"humidity_known\":true") != std::string::npos;
            if(device.airQuality.present()) device.airQuality.ensure().known = line.find("\"air_quality_known\":true") != std::string::npos;
            if(device.co2.present()) device.co2.ensure().known = line.find("\"co2_known\":true") != std::string::npos;
            if(device.pm25.present()) device.pm25.ensure().known = line.find("\"pm25_known\":true") != std::string::npos;
            if(device.light.present()) device.light.ensure().known = line.find("\"light_known\":true") != std::string::npos;
            if(device.brightness.present()) device.brightness.ensure().known = line.find("\"brightness_known\":true") != std::string::npos;
            if(device.occupancy.present()) device.occupancy.ensure().occupied = line.find("\"occupied\":true") != std::string::npos;
            if(device.light.present()) device.light.ensure().on = line.find("\"light_on\":true") != std::string::npos;
            if(device.actor.present()) device.actor.ensure().known = line.find("\"actor_known\":true") != std::string::npos;
            if(device.actor.present()) device.actor.ensure().on = line.find("\"actor_on\":true") != std::string::npos;
            if(device.power.present()) device.power.ensure().known = line.find("\"power_known\":true") != std::string::npos;
            device.hasLowTemperatureAlert = line.find("\"has_low_temperature_alert\":true") != std::string::npos;
            device.hasHighTemperatureAlert = line.find("\"has_high_temperature_alert\":true") != std::string::npos;
            const char* value;
            value = numericField(line, "temperature_celsius");
            if(device.temperature.present()) if (value != 0) device.temperature.ensure().celsius = atof(value);
            value = numericField(line, "power_watts");
            if(device.power.present()) if (value != 0) device.power.ensure().watts = atof(value);
            value = numericField(line, "battery_percent");
            if(device.battery.present()) if (value != 0) device.battery.ensure().percent = atoi(value);
            value = numericField(line, "humidity_percent");
            if(device.humidity.present()) if (value != 0) device.humidity.ensure().percent = atof(value);
            value = numericField(line, "co2_ppm");
            if(device.co2.present()) if (value != 0) device.co2.ensure().ppm = atof(value);
            value = numericField(line, "pm25_micrograms_m3");
            if(device.pm25.present()) if (value != 0) device.pm25.ensure().microgramsM3 = atof(value);
            value = numericField(line, "link_quality");
            if (value != 0) device.zigbee.ensure().linkQuality = atoi(value);
            value = numericField(line, "wifi_signal_percent");
            if(device.wifiSignal.present()) if (value != 0) device.wifiSignal.ensure().percent = atoi(value);
            value = numericField(line, "brightness_percent");
            if(device.brightness.present()) if (value != 0) device.brightness.ensure().percent = atoi(value);
            value = numericField(line, "last_seen");
            if (value != 0) device.lastSeen = atol(value);
            value = numericField(line, "ias_zone_id");
            if (value != 0) {
                long zoneId = strtol(value, 0, 10);
                if (zoneId >= -1 && zoneId <= 254) device.zigbee.ensure().iasZoneId = static_cast<int>(zoneId);
            }
            value = numericField(line, "low_temperature_alert");
            if (value != 0) device.lowTemperatureAlert = atof(value);
            value = numericField(line, "high_temperature_alert");
            if (value != 0) device.highTemperatureAlert = atof(value);
            value = numericField(line, "contact_initial_alert_minutes");
            if (value != 0) device.contactInitialAlertMinutes = strtoul(value, 0, 10);
            value = numericField(line, "contact_reminder_alert_minutes");
            if (value != 0) device.contactReminderAlertMinutes = strtoul(value, 0, 10);
            value = numericField(line, "contact_trigger_cooldown_seconds");
            if (value != 0) device.contactTriggerCooldownSeconds = strtoul(value, 0, 10);
            device.contactTriggerMode = line.find("\"contact_trigger_mode\":true") != std::string::npos;
            value = numericField(line, "sensor_refresh_interval_minutes");
            if (value != 0) device.sensorRefreshIntervalMinutes = strtoul(value, 0, 10);
            value = numericField(line, "matter_endpoint");
            if (value != 0) device.matter.ensure().endpoint = static_cast<int>(strtol(value, 0, 10));
            jsonStringField(line, "matter_node", &device.matter.ensure().node);
            jsonStringField(line, "matter_state", &device.matter.ensure().state);
            if (device.source == "matter" && device.matter.read().state.empty()) device.matter.ensure().state = "complete";
            jsonStringField(line, "matter_metadata_state", &device.matter.ensure().metadataState);
            if (device.source == "matter" && device.matter.read().metadataState != "missing") device.matter.ensure().metadataState = "stale";
            if(device.airQuality.present()) jsonStringField(line, "air_quality", &device.airQuality.ensure().value);
            if(device.co2.present()) jsonStringField(line, "co2_unit", &device.co2.ensure().unit);
            if(device.pm25.present()) jsonStringField(line, "pm25_unit", &device.pm25.ensure().unit);
            if (device.source == "matter") {
                std::string encodedServers;
                if (device.matter.read().node.size() != 16 || device.matter.read().endpoint < -1 ||
                    !jsonStringField(line, "matter_servers", &encodedServers) ||
                    !decodeMatterServers(encodedServers, &device.matter.ensure().servers)) {
                    *error = "invalid Matter endpoint state";
                    return false;
                }
                std::string encodedClients;
                if (jsonStringField(line, "matter_clients", &encodedClients) &&
                    !decodeMatterServers(encodedClients, &device.matter.ensure().clients)) {
                    *error = "invalid Matter endpoint client clusters";
                    return false;
                }
                std::string encodedTypes;
                if (jsonStringField(line, "matter_device_types", &encodedTypes) &&
                    !decodeMatterServers(encodedTypes, &device.matter.ensure().deviceTypes)) {
                    *error = "invalid Matter endpoint device types";
                    return false;
                }
            }
            {
                static const char* alertNames[] = {"temperature", "humidity", "battery", "link_quality", "wifi_signal", "brightness", "power", "co2", "pm25"};
                for (unsigned int alertIndex = 0; alertIndex < 9; ++alertIndex) {
                    std::string prefix = std::string("alert_") + alertNames[alertIndex];
                    const char* low = numericField(line, (prefix + "_low").c_str());
                    const char* high = numericField(line, (prefix + "_high").c_str());
                    const char* hysteresis = numericField(line, (prefix + "_hysteresis").c_str());
                    if (low == 0 && high == 0 && hysteresis == 0) continue;
                    DeviceValueAlert alert;
                    alert.hasLow = line.find("\"" + prefix + "_has_low\":true") != std::string::npos;
                    alert.hasHigh = line.find("\"" + prefix + "_has_high\":true") != std::string::npos;
                    if (low != 0) alert.low = atof(low);
                    if (high != 0) alert.high = atof(high);
                    if (hysteresis != 0) alert.hysteresis = atof(hysteresis);
                    jsonStringField(line, (prefix + "_unit").c_str(), &alert.unit);
                    device.valueAlerts[alertNames[alertIndex]] = alert;
                }
            }
            {
                std::string encodedDescriptors;
                if (jsonStringField(line, "endpoint_descriptors", &encodedDescriptors) && !encodedDescriptors.empty() &&
                    !decodeEndpointDescriptors(encodedDescriptors, &device.zigbee.ensure().endpoints)) {
                    *error = "invalid endpoint descriptor state";
                    return false;
                }
            }
            {
                std::string encodedNodeDescriptor;
                if (jsonStringField(line, "node_descriptor", &encodedNodeDescriptor) && !encodedNodeDescriptor.empty() &&
                    (!hexDecode(encodedNodeDescriptor, &device.zigbee.ensure().nodeDescriptor) || device.zigbee.read().nodeDescriptor.size() != 13)) {
                    *error = "invalid node descriptor state";
                    return false;
                }
            }
            device.zigbee.ensure().initialInterviewComplete = line.find("\"initial_interview_complete\":") == std::string::npos
                ? !device.zigbee.read().endpoints.empty() : line.find("\"initial_interview_complete\":true") != std::string::npos;
            if(line.find("\"has_temperature\":true")==std::string::npos)device.temperature.reset();
            if(line.find("\"has_humidity\":true")==std::string::npos)device.humidity.reset();
            if(line.find("\"has_contact\":true")==std::string::npos)device.contact.reset();
            if(line.find("\"has_water_leak\":true")==std::string::npos)device.waterLeak.reset();
            if(line.find("\"has_air_quality\":true")==std::string::npos)device.airQuality.reset();
            if(line.find("\"has_co2\":true")==std::string::npos)device.co2.reset();
            if(line.find("\"has_pm25\":true")==std::string::npos)device.pm25.reset();
            if(line.find("\"has_occupancy\":true")==std::string::npos)device.occupancy.reset();
            if(line.find("\"has_battery\":true")==std::string::npos)device.battery.reset();
            if(line.find("\"has_light\":true")==std::string::npos)device.light.reset();
            if(line.find("\"has_brightness\":true")==std::string::npos)device.brightness.reset();
            if(line.find("\"has_remote\":true")==std::string::npos)device.remote.reset();
            if(line.find("\"has_actor\":true")==std::string::npos)device.actor.reset();
            if(line.find("\"has_power\":true")==std::string::npos)device.power.reset();
            if(line.find("\"has_wifi_signal\":true")==std::string::npos)device.wifiSignal.reset();
            device.normalizeComponents();
            parsedDevices.push_back(device);
        }
    }
    *devices = parsedDevices;
    return true;
}

namespace {
const size_t cacheLimit=4*1024*1024;
using Proto::Value;
bool cacheInteger(const Value& v,int64_t low,int64_t high,int64_t* n){
    if(v.type==Value::UInt){if(v.u>static_cast<uint64_t>(high))return false;*n=static_cast<int64_t>(v.u);}
    else if(v.type==Value::Int)*n=v.i;else return false;
    return *n>=low&&*n<=high;
}
bool cacheDouble(const Value& v,double low,double high,double* n){
    if(v.type==Value::Float)*n=v.f;else if(v.type==Value::UInt)*n=static_cast<double>(v.u);else if(v.type==Value::Int)*n=static_cast<double>(v.i);else return false;
    return std::isfinite(*n)&&*n>=low&&*n<=high;
}
template<class T> bool cacheReadArray(const Value* v,std::vector<T>* out,uint64_t max,size_t count){
    if(!v)return true;
    if(v->type!=Value::Array||v->items.size()>count)return false;
    for(size_t i=0;i<v->items.size();++i){const Value& x=v->items[i];if(x.type!=Value::UInt||x.u>max)return false;out->push_back(static_cast<T>(x.u));}return true;
}
bool cacheDeviceRead(const Value& v,Device* d){
    if(v.type!=Value::Map)return false;
    {const Value* flag=Proto::field(v,"has_temperature");if(flag && flag->type!=Value::Bool)return false;d->temperature.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_humidity");if(flag && flag->type!=Value::Bool)return false;d->humidity.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_contact");if(flag && flag->type!=Value::Bool)return false;d->contact.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_water_leak");if(flag && flag->type!=Value::Bool)return false;d->waterLeak.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_air_quality");if(flag && flag->type!=Value::Bool)return false;d->airQuality.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_co2");if(flag && flag->type!=Value::Bool)return false;d->co2.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_pm25");if(flag && flag->type!=Value::Bool)return false;d->pm25.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_occupancy");if(flag && flag->type!=Value::Bool)return false;d->occupancy.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_battery");if(flag && flag->type!=Value::Bool)return false;d->battery.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_light");if(flag && flag->type!=Value::Bool)return false;d->light.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_brightness");if(flag && flag->type!=Value::Bool)return false;d->brightness.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_remote");if(flag && flag->type!=Value::Bool)return false;d->remote.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_actor");if(flag && flag->type!=Value::Bool)return false;d->actor.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_power");if(flag && flag->type!=Value::Bool)return false;d->power.setPresent(flag && flag->u);}
    {const Value* flag=Proto::field(v,"has_wifi_signal");if(flag && flag->type!=Value::Bool)return false;d->wifiSignal.setPresent(flag && flag->u);}
    for(size_t i=0;i<v.keys.size();++i){const std::string& key=v.keys[i];const Value& x=v.items[i];int64_t integer=0;double number=0;
#define CACHE_TEXT(name,read,write,guard) if(key==name){if(x.type!=Value::Text||x.text.size()>65536)return false;const std::string& value=x.text;if(guard){d->write;}}
#define CACHE_BOOL(name,read,write,guard) if(key==name){if(x.type!=Value::Bool)return false;const bool value=x.u!=0;if(guard){d->write;}}
#define CACHE_INT(name,read,write,guard,low,high) if(key==name){if(!cacheInteger(x,low,high,&integer))return false;const int64_t value=integer;if(guard){d->write;}}
#define CACHE_NUMBER(name,read,write,guard,low,high) if(key==name){if(!cacheDouble(x,low,high,&number))return false;const double value=number;if(guard){d->write;}}
#include "device_cache_fields.inc"
#undef CACHE_TEXT
#undef CACHE_BOOL
#undef CACHE_INT
#undef CACHE_NUMBER
    }
    const Value* endpoints=Proto::field(v,"endpoint_descriptors");
    if(endpoints){if(endpoints->type!=Value::Array||endpoints->items.size()>255)return false;for(size_t i=0;i<endpoints->items.size();++i){const Value& x=endpoints->items[i];EndpointDescriptor e;const char* keys[]={"endpoint","profile_id","device_id","device_version"};unsigned long vals[4];for(unsigned j=0;j<4;++j){const Value* f=Proto::field(x,keys[j]);if(!f||f->type!=Value::UInt||f->u>((j==0||j==3)?255U:65535U))return false;vals[j]=f->u;}e.endpoint=vals[0];e.profileId=vals[1];e.deviceId=vals[2];e.deviceVersion=vals[3];if(!cacheReadArray(Proto::field(x,"input_clusters"),&e.inputClusters,65535,255)||!cacheReadArray(Proto::field(x,"output_clusters"),&e.outputClusters,65535,255))return false;d->zigbee.ensure().endpoints.push_back(e);}}
    const Value* bytes=Proto::field(v,"node_descriptor");if(bytes){if(bytes->type!=Value::Binary)return false;d->zigbee.ensure().nodeDescriptor.assign(bytes->text.begin(),bytes->text.end());}
    if(!cacheReadArray(Proto::field(v,"matter_servers"),&d->matter.ensure().servers,0xffffffffUL,64)||!cacheReadArray(Proto::field(v,"matter_clients"),&d->matter.ensure().clients,0xffffffffUL,64)||!cacheReadArray(Proto::field(v,"matter_device_types"),&d->matter.ensure().deviceTypes,0xffffffffUL,64))return false;
    const Value* alerts=Proto::field(v,"alerts");
    if(alerts){if(alerts->type!=Value::Array||alerts->items.size()>9)return false;for(size_t i=0;i<alerts->items.size();++i){const Value& a=alerts->items[i];const Value* name=Proto::field(a,"name");if(!name||name->type!=Value::Text||d->valueAlerts.count(name->text))return false;static const char* allowed[]={"temperature","humidity","battery","link_quality","wifi_signal","brightness","power","co2","pm25"};bool found=false;for(unsigned j=0;j<9;++j)if(name->text==allowed[j])found=true;if(!found)return false;DeviceValueAlert alert;const Value* low=Proto::field(a,"low_enabled");const Value* high=Proto::field(a,"high_enabled");const Value* l=Proto::field(a,"low");const Value* h=Proto::field(a,"high");const Value* hysteresis=Proto::field(a,"hysteresis");const Value* unit=Proto::field(a,"unit");if(!low||low->type!=Value::Bool||!high||high->type!=Value::Bool||!l||!h||!hysteresis||!unit||unit->type!=Value::Text||!cacheDouble(*l,-1e308,1e308,&alert.low)||!cacheDouble(*h,-1e308,1e308,&alert.high)||!cacheDouble(*hysteresis,0,1e308,&alert.hysteresis))return false;alert.hasLow=low->u;alert.hasHigh=high->u;alert.unit=unit->text;d->valueAlerts[name->text]=alert;}}
    if(!Proto::field(v,"has_temperature") || !Proto::field(v,"has_temperature")->u)d->temperature.reset();
    if(!Proto::field(v,"has_humidity") || !Proto::field(v,"has_humidity")->u)d->humidity.reset();
    if(!Proto::field(v,"has_contact") || !Proto::field(v,"has_contact")->u)d->contact.reset();
    if(!Proto::field(v,"has_water_leak") || !Proto::field(v,"has_water_leak")->u)d->waterLeak.reset();
    if(!Proto::field(v,"has_air_quality") || !Proto::field(v,"has_air_quality")->u)d->airQuality.reset();
    if(!Proto::field(v,"has_co2") || !Proto::field(v,"has_co2")->u)d->co2.reset();
    if(!Proto::field(v,"has_pm25") || !Proto::field(v,"has_pm25")->u)d->pm25.reset();
    if(!Proto::field(v,"has_occupancy") || !Proto::field(v,"has_occupancy")->u)d->occupancy.reset();
    if(!Proto::field(v,"has_battery") || !Proto::field(v,"has_battery")->u)d->battery.reset();
    if(!Proto::field(v,"has_light") || !Proto::field(v,"has_light")->u)d->light.reset();
    if(!Proto::field(v,"has_brightness") || !Proto::field(v,"has_brightness")->u)d->brightness.reset();
    if(!Proto::field(v,"has_remote") || !Proto::field(v,"has_remote")->u)d->remote.reset();
    if(!Proto::field(v,"has_actor") || !Proto::field(v,"has_actor")->u)d->actor.reset();
    if(!Proto::field(v,"has_power") || !Proto::field(v,"has_power")->u)d->power.reset();
    if(!Proto::field(v,"has_wifi_signal") || !Proto::field(v,"has_wifi_signal")->u)d->wifiSignal.reset();
    d->normalizeComponents();
    if(d->ieeeAddress.empty()||(d->source!="zigbee"&&d->source!="matter"&&d->source!="shelly"&&d->source!="fritz_box"))return false;
    if(d->source=="matter"){if(d->matter.read().node.size()!=16)return false;if(d->matter.read().state.empty())d->matter.ensure().state="complete";if(d->matter.read().metadataState!="missing")d->matter.ensure().metadataState="stale";}
    return true;
}
typedef bool (*EncodeCallback)(pb_ostream_t*,const pb_field_t*,void* const*);
void bindCallback(pb_callback_t* callback,EncodeCallback fn,const void* arg){callback->funcs.encode=fn;callback->arg=const_cast<void*>(arg);}
bool typedUtf8(const std::string& s){
 for(size_t at=0;at<s.size();){unsigned char c=s[at++];if(c<128)continue;unsigned n=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:0;if(!n||at+n>s.size())return false;unsigned v=c&((1U<<(6-n))-1);for(unsigned i=0;i<n;++i){unsigned char b=s[at++];if((b&192)!=128)return false;v=(v<<6)|(b&63);}if(v<(n==1?128U:n==2?2048U:65536U)||v>0x10ffffU||(v>=0xd800&&v<=0xdfff))return false;}return true;
}
bool typedText(pb_ostream_t* stream,const pb_field_t* f,void* const* arg){const std::string& text=*static_cast<const std::string*>(*arg);return text.size()<=65536&&typedUtf8(text)&&pb_encode_tag_for_field(stream,f)&&pb_encode_string(stream,reinterpret_cast<const pb_byte_t*>(text.data()),text.size());}
bool typedBytes(pb_ostream_t* stream,const pb_field_t* f,void* const* arg){const std::vector<unsigned char>& b=*static_cast<const std::vector<unsigned char>*>(*arg);return b.size()<=65536&&pb_encode_tag_for_field(stream,f)&&pb_encode_string(stream,b.empty()?0:&b[0],b.size());}
template<class T> bool typedUints(pb_ostream_t* stream,const pb_field_t* f,void* const* arg){const std::vector<T>& values=*static_cast<const std::vector<T>*>(*arg);if(values.size()>512)return false;for(size_t i=0;i<values.size();++i)if(!pb_encode_tag_for_field(stream,f)||!pb_encode_varint(stream,values[i]))return false;return true;}
bool typedEndpoints(pb_ostream_t* stream,const pb_field_t* f,void* const* arg){
 const std::vector<EndpointDescriptor>& endpoints=*static_cast<const std::vector<EndpointDescriptor>*>(*arg);if(endpoints.size()>512)return false;
 for(size_t i=0;i<endpoints.size();++i){const EndpointDescriptor& d=endpoints[i];microhad_CachedEndpoint v=microhad_CachedEndpoint_init_zero;
 v.has_endpoint=true;v.endpoint=d.endpoint;v.has_profile_id=true;v.profile_id=d.profileId;v.has_device_id=true;v.device_id=d.deviceId;v.has_device_version=true;v.device_version=d.deviceVersion;
 bindCallback(&v.input_clusters,typedUints<unsigned short>,&d.inputClusters);bindCallback(&v.output_clusters,typedUints<unsigned short>,&d.outputClusters);
 if(!pb_encode_tag_for_field(stream,f)||!pb_encode_submessage(stream,microhad_CachedEndpoint_fields,&v))return false;
 }return true;
}
bool typedAlerts(pb_ostream_t* stream,const pb_field_t* f,void* const* arg){
 const std::map<std::string,DeviceValueAlert>& alerts=*static_cast<const std::map<std::string,DeviceValueAlert>*>(*arg);if(alerts.size()>512)return false;
 for(std::map<std::string,DeviceValueAlert>::const_iterator it=alerts.begin();it!=alerts.end();++it){const DeviceValueAlert& a=it->second;microhad_CachedAlert v=microhad_CachedAlert_init_zero;
 if(!std::isfinite(a.low)||!std::isfinite(a.high)||!std::isfinite(a.hysteresis))return false;
 bindCallback(&v.name,typedText,&it->first);bindCallback(&v.unit,typedText,&a.unit);
 v.has_low_enabled=true;v.low_enabled=a.hasLow;v.has_high_enabled=true;v.high_enabled=a.hasHigh;v.has_low=true;v.low=a.low;v.has_high=true;v.high=a.high;v.has_hysteresis=true;v.hysteresis=a.hysteresis;
 if(!pb_encode_tag_for_field(stream,f)||!pb_encode_submessage(stream,microhad_CachedAlert_fields,&v))return false;
 }return true;
}
bool typedDevices(pb_ostream_t* stream,const pb_field_t* f,void* const* arg){
 const std::vector<Device>& devices=*static_cast<const std::vector<Device>*>(*arg);
 for(size_t i=0;i<devices.size();++i){const Device& d=devices[i];microhad_CachedDevice v=microhad_CachedDevice_init_zero;
#include "../../protocol/generated/device_cache_encode.inc"
 bindCallback(&v.endpoint_descriptors,typedEndpoints,&d.zigbee.read().endpoints);bindCallback(&v.node_descriptor,typedBytes,&d.zigbee.read().nodeDescriptor);
 bindCallback(&v.matter_servers,typedUints<unsigned int>,&d.matter.read().servers);bindCallback(&v.matter_clients,typedUints<unsigned int>,&d.matter.read().clients);bindCallback(&v.matter_device_types,typedUints<unsigned int>,&d.matter.read().deviceTypes);bindCallback(&v.alerts,typedAlerts,&d.valueAlerts);
 if(!pb_encode_tag_for_field(stream,f)||!pb_encode_submessage(stream,microhad_CachedDevice_fields,&v))return false;
 }return true;
}
bool encodeCache(const std::vector<Device>& devices,Proto::Bytes* out){
 microhad_DeviceCache cache=microhad_DeviceCache_init_zero;cache.has_version=true;cache.version=1;bindCallback(&cache.devices,typedDevices,&devices);
 out->resize(65536);
 for(;;){pb_ostream_t stream=pb_ostream_from_buffer(&(*out)[0],out->size());if(pb_encode(&stream,microhad_DeviceCache_fields,&cache)){out->resize(stream.bytes_written);return true;}if(out->size()==cacheLimit||std::strcmp(PB_GET_ERROR(&stream),"stream full")!=0)return false;out->resize(std::min(cacheLimit,out->size()*2));}
}

}

bool saveDeviceCache(const std::string& filename,const std::vector<Device>& devices,std::string* error){
    if(error)error->clear();
    if(devices.size()>4096){if(error)*error="too many devices for protobuf cache";return false;}
    Proto::Bytes bytes;if(!encodeCache(devices,&bytes)){if(error)*error="cannot encode device protobuf cache";return false;}
    std::string pattern=filename+".tmp.XXXXXX";std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
    int fd=mkstemp(&name[0]);bool ok=fd>=0;if(ok)ok=fchmod(fd,0600)==0;size_t at=0;
    while(ok&&at<bytes.size()){ssize_t n=write(fd,&bytes[at],bytes.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0)ok=false;else at+=n;}
    if(fd>=0&&close(fd)!=0)ok=false;
    if(ok)ok=rename(&name[0],filename.c_str())==0;
    if(!ok){unlink(&name[0]);if(error)*error="cannot atomically write device protobuf cache";}return ok;
}
bool loadDeviceCache(const std::string& filename,std::vector<Device>* devices,std::string* error){
    if(error)error->clear();
    int fd=open(filename.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0){if(errno==ENOENT){devices->clear();return true;}if(error)*error="cannot open device protobuf cache";return false;}
    struct stat st;bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<=static_cast<off_t>(cacheLimit);
    Proto::Bytes bytes(ok?static_cast<size_t>(st.st_size):1);size_t at=0;while(ok&&at<bytes.size()){ssize_t n=read(fd,&bytes[at],bytes.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0)ok=false;else at+=n;}close(fd);
    pb_istream_t stream=pb_istream_from_buffer(&bytes[0],bytes.size());bool version=false;std::vector<Device> parsed;std::set<std::string> ids;std::set<unsigned short> nodes;
    while(ok&&stream.bytes_left){pb_wire_type_t wt;uint32_t tag;bool eof=false;ok=pb_decode_tag(&stream,&wt,&tag,&eof)&&tag>0;if(!ok)break;
        if(tag==1){uint64_t n;ok=wt==PB_WT_VARINT&&!version&&pb_decode_varint(&stream,&n)&&n==1;version=true;}
        else if(tag==2){pb_istream_t sub;ok=wt==PB_WT_STRING&&parsed.size()<4096&&pb_make_string_substream(&stream,&sub);if(!ok)break;Proto::Bytes record(sub.bytes_left);ok=!record.empty()&&pb_read(&sub,&record[0],record.size())&&pb_close_string_substream(&stream,&sub);Value v;unsigned unknown=0;Device d;ok=ok&&Proto::decode("CachedDevice",record,&v,cacheLimit,&unknown)&&cacheDeviceRead(v,&d)&&ids.insert(d.ieeeAddress).second;if(ok&&d.source=="zigbee"&&d.zigbee.read().networkAddress!=0xffff)ok=nodes.insert(d.zigbee.read().networkAddress).second;if(ok)parsed.push_back(d);}
        else ok=pb_skip_field(&stream,wt);
    }
    if(!ok||!version){if(error)*error="invalid device protobuf cache";return false;}devices->swap(parsed);return true;
}

namespace {
std::vector<std::string> tabFields(const std::string& line) {
    std::vector<std::string> result;
    std::string::size_type start = 0;
    while (start <= line.size()) {
        std::string::size_type end = line.find('\t', start);
        result.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}
SceneGroup* sceneGroupById(std::vector<SceneGroup>* groups, unsigned short id) {
    for (unsigned long i = 0; i < groups->size(); ++i) if ((*groups)[i].id == id) return &(*groups)[i];
    return 0;
}
Scene* sceneById(SceneGroup* group, unsigned char id) {
    if (group == 0) return 0;
    for (unsigned long i = 0; i < group->scenes.size(); ++i) if (group->scenes[i].id == id) return &group->scenes[i];
    return 0;
}

bool parseSceneRecord(const std::vector<std::string>& fields, std::vector<SceneGroup>* groups,
                      std::vector<RemoteSceneBinding>* bindings,
                      std::vector<NativeRemoteBinding>* nativeBindings) {
    unsigned long groupId, sceneId;
    const std::string& type = fields[0];

    if (type == "G") {
        if (fields.size() != 3 || !decimalField(fields[1], 0xfff7, &groupId) ||
            groupId == 0 || fields[2].empty()) return false;
        SceneGroup group;
        group.id = static_cast<unsigned short>(groupId);
        group.name = fields[2];
        groups->push_back(group);
        return true;
    }
    if (type == "M") {
        if (fields.size() != 3 || !decimalField(fields[1], 0xfff7, &groupId) || fields[2].empty()) return false;
        SceneGroup* group = sceneGroupById(groups, static_cast<unsigned short>(groupId));
        if (group == 0) return false;
        group->members.push_back(fields[2]);
        return true;
    }
    if (type == "S") {
        if (fields.size() != 4 || !decimalField(fields[1], 0xfff7, &groupId) ||
            !decimalField(fields[2], 255, &sceneId) || sceneId == 0 || fields[3].empty()) return false;
        SceneGroup* group = sceneGroupById(groups, static_cast<unsigned short>(groupId));
        if (group == 0) return false;
        Scene scene;
        scene.id = static_cast<unsigned char>(sceneId);
        scene.name = fields[3];
        group->scenes.push_back(scene);
        return true;
    }
    if (type == "T") {
        int brightness;
        if (fields.size() != 6 || !decimalField(fields[1], 0xfff7, &groupId) ||
            !decimalField(fields[2], 255, &sceneId) || fields[3].empty() ||
            (fields[4] != "0" && fields[4] != "1") || !brightnessField(fields[5], &brightness)) return false;
        Scene* scene = sceneById(sceneGroupById(groups, static_cast<unsigned short>(groupId)),
                                 static_cast<unsigned char>(sceneId));
        if (scene == 0) return false;
        SceneLightState state;
        state.ieeeAddress = fields[3];
        state.on = fields[4] == "1";
        state.brightnessPercent = brightness;
        scene->states.push_back(state);
        return true;
    }
    if (type == "B") {
        if (fields.size() != 6 || !decimalField(fields[3], 0xfff7, &groupId) || groupId == 0 ||
            fields[1].empty() || (fields[4] != "0" && fields[4] != "1") ||
            (fields[2] != "on" && fields[2] != "off" && fields[2] != "arrow_left" &&
             fields[2] != "arrow_right" && fields[2] != "matter_press" &&
             fields[2] != "matter_hold" && fields[2] != "matter_release" &&
             fields[2] != "matter_multi_press")) return false;
        RemoteSceneBinding binding;
        binding.ieeeAddress = fields[1];
        binding.action = fields[2];
        binding.groupId = static_cast<unsigned short>(groupId);
        binding.off = fields[4] == "1";
        std::stringstream ids(fields[5]);
        std::string id;
        while (std::getline(ids, id, ',')) {
            if (!decimalField(id, 255, &sceneId) || sceneId == 0) return false;
            binding.sceneIds.push_back(static_cast<unsigned char>(sceneId));
        }
        bindings->push_back(binding);
        return true;
    }
    if (type == "N") {
        if (fields.size() != 3 || fields[1].empty() ||
            !decimalField(fields[2], 0xfff7, &groupId) || groupId == 0) return false;
        NativeRemoteBinding binding;
        binding.ieeeAddress = fields[1];
        binding.groupId = static_cast<unsigned short>(groupId);
        nativeBindings->push_back(binding);
        return true;
    }
    return false;
}

bool uniqueSceneGroups(const std::vector<SceneGroup>& groups) {
    for (unsigned long i = 0; i < groups.size(); ++i) {
        for (unsigned long j = 0; j < i; ++j)
            if (groups[i].id == groups[j].id) return false;
        for (unsigned long j = 0; j < groups[i].members.size(); ++j)
            for (unsigned long k = 0; k < j; ++k)
                if (groups[i].members[j] == groups[i].members[k]) return false;
        for (unsigned long j = 0; j < groups[i].scenes.size(); ++j) {
            const Scene& scene = groups[i].scenes[j];
            for (unsigned long k = 0; k < j; ++k)
                if (scene.id == groups[i].scenes[k].id) return false;
            for (unsigned long k = 0; k < scene.states.size(); ++k)
                for (unsigned long n = 0; n < k; ++n)
                    if (scene.states[k].ieeeAddress == scene.states[n].ieeeAddress) return false;
        }
    }
    return true;
}
}

bool saveSceneConfiguration(const std::string& filename, const std::vector<SceneGroup>& groups,
                            const std::vector<RemoteSceneBinding>& bindings,
                            const std::vector<NativeRemoteBinding>& nativeBindings, std::string* error) {
    std::string temporary = filename + ".tmp";
    std::ofstream output(temporary.c_str());
    if (!output) { *error = "cannot open scene configuration for writing"; return false; }
    output << "# microhad-zigbee-scenes-v2\n";
    for (unsigned long i = 0; i < groups.size(); ++i) {
        const SceneGroup& group = groups[i];
        output << "G\t" << group.id << "\t" << group.name << "\n";
        for (unsigned long j = 0; j < group.members.size(); ++j) output << "M\t" << group.id << "\t" << group.members[j] << "\n";
        for (unsigned long j = 0; j < group.scenes.size(); ++j) {
            const Scene& scene = group.scenes[j];
            output << "S\t" << group.id << "\t" << static_cast<unsigned int>(scene.id) << "\t" << scene.name << "\n";
            for (unsigned long k = 0; k < scene.states.size(); ++k) {
                const SceneLightState& state = scene.states[k];
                output << "T\t" << group.id << "\t" << static_cast<unsigned int>(scene.id) << "\t" << state.ieeeAddress
                       << "\t" << (state.on ? 1 : 0) << "\t" << state.brightnessPercent << "\n";
            }
        }
    }
    for (unsigned long i = 0; i < bindings.size(); ++i) {
        output << "B\t" << bindings[i].ieeeAddress << "\t" << bindings[i].action << "\t" << bindings[i].groupId
               << "\t" << (bindings[i].off ? 1 : 0) << "\t";
        for (unsigned long j = 0; j < bindings[i].sceneIds.size(); ++j) {
            if (j) output << ',';
            output << static_cast<unsigned int>(bindings[i].sceneIds[j]);
        }
        output << "\n";
    }
    for (unsigned long i = 0; i < nativeBindings.size(); ++i)
        output << "N\t" << nativeBindings[i].ieeeAddress << "\t" << nativeBindings[i].groupId << "\n";
    output.close();
    if (!output || rename(temporary.c_str(), filename.c_str()) != 0) { unlink(temporary.c_str()); *error = "cannot atomically save scene configuration"; return false; }
    return true;
}

bool loadSceneConfiguration(const std::string& filename, std::vector<SceneGroup>* groups,
                            std::vector<RemoteSceneBinding>* bindings,
                            std::vector<NativeRemoteBinding>* nativeBindings, std::string* error) {
    std::ifstream input(filename.c_str());
    std::string line;
    std::vector<SceneGroup> parsedGroups;
    std::vector<RemoteSceneBinding> parsedBindings;
    std::vector<NativeRemoteBinding> parsedNativeBindings;
    if (!input) { groups->clear(); bindings->clear(); nativeBindings->clear(); return true; }
    while (std::getline(input, line)) {
        std::vector<std::string> fields = tabFields(line);
        if (fields[0].empty() || fields[0][0] == '#') continue;
        if (!parseSceneRecord(fields, &parsedGroups, &parsedBindings, &parsedNativeBindings)) {
            *error = "invalid scene configuration";
            return false;
        }
    }
    if (!uniqueSceneGroups(parsedGroups)) {
        *error = "invalid scene configuration";
        return false;
    }
    *groups = parsedGroups;
    *bindings = parsedBindings;
    *nativeBindings = parsedNativeBindings;
    return true;
}

bool saveSceneConfiguration(const std::string& filename, const std::vector<SceneGroup>& groups,
                            const std::vector<RemoteSceneBinding>& bindings, std::string* error) {
    return saveSceneConfiguration(filename, groups, bindings, std::vector<NativeRemoteBinding>(), error);
}

bool loadSceneConfiguration(const std::string& filename, std::vector<SceneGroup>* groups,
                            std::vector<RemoteSceneBinding>* bindings, std::string* error) {
    std::vector<NativeRemoteBinding> ignored;
    return loadSceneConfiguration(filename, groups, bindings, &ignored, error);
}

bool saveNetworkState(const std::string& filename, const NetworkState& network, std::string* error) {
    std::string temporary = filename + ".tmp";
    std::ofstream output(temporary.c_str());
    if (!network.formed || network.extendedPan.size() != 8 || network.networkKey.size() != 16 ||
        network.preconfiguredKey.size() != 16) { *error = "invalid network state"; return false; }
    if (!output) { *error = "cannot open network state for writing"; return false; }
    output << "{\"version\":1,\"formed\":true,\"pan_id\":" << network.panId << ",\"channel\":"
           << static_cast<unsigned int>(network.channel) << ",\"extended_pan\":\""
           << hexEncode(network.extendedPan) << "\",\"network_key\":\"" << hexEncode(network.networkKey)
           << "\",\"preconfigured_key\":\"" << hexEncode(network.preconfiguredKey) << "\"}\n";
    output.close();
    if (!output || chmod(temporary.c_str(), 0600) != 0 || rename(temporary.c_str(), filename.c_str()) != 0) {
        unlink(temporary.c_str()); *error = "cannot atomically save network state"; return false;
    }
    return true;
}

bool loadNetworkState(const std::string& filename, NetworkState* network, std::string* error) {
    std::ifstream input(filename.c_str());
    std::string content;
    std::string extendedPan, networkKey, preconfiguredKey;
    std::string::size_type first, last;
    long panId, channel, version;
    NetworkState decoded;
    if (!input) { *network = NetworkState(); return true; }
    content.assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    first = content.find_first_not_of(" \t\r\n");
    last = content.find_last_not_of(" \t\r\n");
    if (first == std::string::npos || content[first] != '{' || content[last] != '}' ||
        content.find("\"formed\":true") == std::string::npos || !jsonLongField(content, "version", &version) || version != 1 ||
        !jsonStringField(content, "extended_pan", &extendedPan) ||
        !jsonStringField(content, "network_key", &networkKey) || !jsonStringField(content, "preconfigured_key", &preconfiguredKey) ||
        !hexDecode(extendedPan, &decoded.extendedPan) || !hexDecode(networkKey, &decoded.networkKey) ||
        !hexDecode(preconfiguredKey, &decoded.preconfiguredKey) || decoded.extendedPan.size() != 8 ||
        decoded.networkKey.size() != 16 || decoded.preconfiguredKey.size() != 16 ||
        !jsonLongField(content, "pan_id", &panId) || !jsonLongField(content, "channel", &channel)) {
        *error = "invalid network state"; return false;
    }
    if (panId <= 0 || panId >= 0xffff || channel < 11 || channel > 26) {
        *error = "invalid network state"; return false;
    }
    decoded.panId = static_cast<unsigned short>(panId);
    decoded.channel = static_cast<unsigned char>(channel);
    decoded.formed = true;
    *network = decoded;
    return true;
}
