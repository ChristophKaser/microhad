#include "storage.h"
#include "gateway.h"
#include "json_util.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

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
               << "\",\"external_id\":\"" << jsonEscape(device.externalId)
               << "\",\"component\":\"" << jsonEscape(device.component)
               << "\",\"network_address\":" << device.networkAddress << ",\"name\":\"" << jsonEscape(device.name) << "\",\"kind\":\""
               << jsonEscape(device.kind) << "\",\"contact_open\":" << (device.contactOpen ? "true" : "false")
               << ",\"has_contact\":" << (device.hasContact ? "true" : "false")
               << ",\"contact_known\":" << (device.contactKnown ? "true" : "false")
               << ",\"temperature_celsius\":" << device.temperatureCelsius << ",\"has_temperature\":"
               << (device.hasTemperature ? "true" : "false") << ",\"humidity_percent\":" << device.humidityPercent
               << ",\"temperature_known\":" << (device.temperatureKnown ? "true" : "false")
               << ",\"has_humidity\":" << (device.hasHumidity ? "true" : "false") << ",\"occupied\":"
               << (device.occupied ? "true" : "false") << ",\"has_occupancy\":"
               << (device.hasOccupancy ? "true" : "false") << ",\"battery_percent\":" << device.batteryPercent
               << ",\"has_battery\":" << (device.hasBattery ? "true" : "false") << ",\"link_quality\":"
               << device.linkQuality << ",\"has_wifi_signal\":" << (device.hasWifiSignal ? "true" : "false")
               << ",\"wifi_signal_percent\":" << device.wifiSignalPercent << ",\"last_seen\":" << device.lastSeen << ",\"ias_zone_id\":" << device.iasZoneId
               << ",\"has_light\":" << (device.hasLight ? "true" : "false") << ",\"light_on\":"
               << (device.lightOn ? "true" : "false") << ",\"has_brightness\":"
               << (device.hasBrightness ? "true" : "false") << ",\"brightness_percent\":" << device.brightnessPercent
               << ",\"has_remote\":" << (device.hasRemote ? "true" : "false")
               << ",\"has_actor\":" << (device.hasActor ? "true" : "false")
               << ",\"actor_known\":" << (device.actorKnown ? "true" : "false")
               << ",\"actor_on\":" << (device.actorOn ? "true" : "false")
               << ",\"has_power\":" << (device.hasPower ? "true" : "false")
               << ",\"power_known\":" << (device.powerKnown ? "true" : "false")
               << ",\"power_watts\":" << device.powerWatts
               << ",\"has_low_temperature_alert\":"
               << (device.hasLowTemperatureAlert ? "true" : "false") << ",\"low_temperature_alert\":"
               << device.lowTemperatureAlert << ",\"has_high_temperature_alert\":"
               << (device.hasHighTemperatureAlert ? "true" : "false") << ",\"high_temperature_alert\":"
               << device.highTemperatureAlert << ",\"contact_initial_alert_minutes\":"
               << device.contactInitialAlertMinutes << ",\"contact_reminder_alert_minutes\":"
               << device.contactReminderAlertMinutes << ",\"sensor_refresh_interval_minutes\":"
               << device.sensorRefreshIntervalMinutes;
        for (std::map<std::string, DeviceValueAlert>::const_iterator alert = device.valueAlerts.begin(); alert != device.valueAlerts.end(); ++alert) {
            output << ",\"alert_" << alert->first << "_has_low\":" << (alert->second.hasLow ? "true" : "false")
                   << ",\"alert_" << alert->first << "_low\":" << alert->second.low
                   << ",\"alert_" << alert->first << "_has_high\":" << (alert->second.hasHigh ? "true" : "false")
                   << ",\"alert_" << alert->first << "_high\":" << alert->second.high
                   << ",\"alert_" << alert->first << "_hysteresis\":" << alert->second.hysteresis;
        }
        output << ",\"manufacturer_name\":\""
               << jsonEscape(device.manufacturerName) << "\",\"model_id\":\"" << jsonEscape(device.modelId)
               << "\",\"software_build_id\":\"" << jsonEscape(device.softwareBuildId)
               << "\",\"endpoint_descriptors\":\"" << encodeEndpointDescriptors(device.endpoints)
               << "\",\"node_descriptor\":\"" << hexEncode(device.nodeDescriptor) << "\"}";
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
                !validOptionalDecimalField(line, "power_watts", 0, 6553.5) ||
                !validOptionalDecimalField(line, "low_temperature_alert", -100, 200) ||
                !validOptionalDecimalField(line, "high_temperature_alert", -100, 200) ||
                !validOptionalIntegerField(line, "battery_percent", -1, 100) ||
                !validOptionalIntegerField(line, "link_quality", 0, 255) ||
                !validOptionalIntegerField(line, "wifi_signal_percent", 0, 100) ||
                !validOptionalIntegerField(line, "brightness_percent", -1, 100) ||
                !validOptionalIntegerField(line, "last_seen", 0, LONG_MAX) ||
                !validOptionalIntegerField(line, "ias_zone_id", -1, 254) ||
                !validOptionalIntegerField(line, "contact_initial_alert_minutes", 0, LONG_MAX) ||
                !validOptionalIntegerField(line, "contact_reminder_alert_minutes", 0, LONG_MAX) ||
                !validOptionalIntegerField(line, "sensor_refresh_interval_minutes", 0, LONG_MAX)) continue;
            bool duplicate = false;
            for (unsigned long previous = 0; previous < parsedDevices.size(); ++previous) {
                if (parsedDevices[previous].ieeeAddress == ieeeAddress ||
                    (networkAddress != 0xffff && parsedDevices[previous].networkAddress == networkAddress)) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            Device device;
            device.ieeeAddress = ieeeAddress;
            jsonStringField(line, "source", &device.source);
            if (device.source != "zigbee" && device.source != "shelly" && device.source != "fritz_box") continue;
            jsonStringField(line, "external_id", &device.externalId);
            jsonStringField(line, "component", &device.component);
            if (addressValue != 0) device.networkAddress = static_cast<unsigned short>(networkAddress);
            const char* fields[] = {"name", "kind", "manufacturer_name", "model_id", "software_build_id"};
            std::string* destinations[] = {&device.name, &device.kind, &device.manufacturerName, &device.modelId, &device.softwareBuildId};
            int index;
            for (index = 0; index < 5; ++index) {
                jsonStringField(line, fields[index], destinations[index]);
            }
            device.contactOpen = line.find("\"contact_open\":true") != std::string::npos;
            device.hasContact = line.find("\"has_contact\":true") != std::string::npos;
            device.contactKnown = line.find("\"contact_known\":true") != std::string::npos;
            device.hasTemperature = line.find("\"has_temperature\":true") != std::string::npos;
            device.temperatureKnown = line.find("\"temperature_known\":true") != std::string::npos;
            device.hasHumidity = line.find("\"has_humidity\":true") != std::string::npos;
            device.hasOccupancy = line.find("\"has_occupancy\":true") != std::string::npos;
            device.occupied = line.find("\"occupied\":true") != std::string::npos;
            device.hasBattery = line.find("\"has_battery\":true") != std::string::npos;
            device.hasWifiSignal = line.find("\"has_wifi_signal\":true") != std::string::npos;
            device.hasLight = line.find("\"has_light\":true") != std::string::npos;
            device.lightOn = line.find("\"light_on\":true") != std::string::npos;
            device.hasBrightness = line.find("\"has_brightness\":true") != std::string::npos;
            device.hasRemote = line.find("\"has_remote\":true") != std::string::npos;
            device.hasActor = line.find("\"has_actor\":true") != std::string::npos;
            device.actorKnown = line.find("\"actor_known\":true") != std::string::npos;
            device.actorOn = line.find("\"actor_on\":true") != std::string::npos;
            device.hasPower = line.find("\"has_power\":true") != std::string::npos;
            device.powerKnown = line.find("\"power_known\":true") != std::string::npos;
            device.hasLowTemperatureAlert = line.find("\"has_low_temperature_alert\":true") != std::string::npos;
            device.hasHighTemperatureAlert = line.find("\"has_high_temperature_alert\":true") != std::string::npos;
            const char* value;
            value = numericField(line, "temperature_celsius");
            if (value != 0) device.temperatureCelsius = atof(value);
            value = numericField(line, "power_watts");
            if (value != 0) device.powerWatts = atof(value);
            value = numericField(line, "battery_percent");
            if (value != 0) device.batteryPercent = atoi(value);
            value = numericField(line, "humidity_percent");
            if (value != 0) device.humidityPercent = atof(value);
            value = numericField(line, "link_quality");
            if (value != 0) device.linkQuality = atoi(value);
            value = numericField(line, "wifi_signal_percent");
            if (value != 0) device.wifiSignalPercent = atoi(value);
            value = numericField(line, "brightness_percent");
            if (value != 0) device.brightnessPercent = atoi(value);
            value = numericField(line, "last_seen");
            if (value != 0) device.lastSeen = atol(value);
            value = numericField(line, "ias_zone_id");
            if (value != 0) {
                long zoneId = strtol(value, 0, 10);
                if (zoneId >= -1 && zoneId <= 254) device.iasZoneId = static_cast<int>(zoneId);
            }
            value = numericField(line, "low_temperature_alert");
            if (value != 0) device.lowTemperatureAlert = atof(value);
            value = numericField(line, "high_temperature_alert");
            if (value != 0) device.highTemperatureAlert = atof(value);
            value = numericField(line, "contact_initial_alert_minutes");
            if (value != 0) device.contactInitialAlertMinutes = strtoul(value, 0, 10);
            value = numericField(line, "contact_reminder_alert_minutes");
            if (value != 0) device.contactReminderAlertMinutes = strtoul(value, 0, 10);
            value = numericField(line, "sensor_refresh_interval_minutes");
            if (value != 0) device.sensorRefreshIntervalMinutes = strtoul(value, 0, 10);
            {
                static const char* alertNames[] = {"temperature", "humidity", "battery", "link_quality", "wifi_signal", "brightness", "power"};
                for (unsigned int alertIndex = 0; alertIndex < 7; ++alertIndex) {
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
                    device.valueAlerts[alertNames[alertIndex]] = alert;
                }
            }
            {
                std::string encodedDescriptors;
                if (jsonStringField(line, "endpoint_descriptors", &encodedDescriptors) && !encodedDescriptors.empty() &&
                    !decodeEndpointDescriptors(encodedDescriptors, &device.endpoints)) {
                    *error = "invalid endpoint descriptor state";
                    return false;
                }
            }
            {
                std::string encodedNodeDescriptor;
                if (jsonStringField(line, "node_descriptor", &encodedNodeDescriptor) && !encodedNodeDescriptor.empty() &&
                    (!hexDecode(encodedNodeDescriptor, &device.nodeDescriptor) || device.nodeDescriptor.size() != 13)) {
                    *error = "invalid node descriptor state";
                    return false;
                }
            }
            parsedDevices.push_back(device);
        }
    }
    *devices = parsedDevices;
    return true;
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
             fields[2] != "arrow_right")) return false;
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
