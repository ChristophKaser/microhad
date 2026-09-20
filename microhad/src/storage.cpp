#include "storage.h"
#include "gateway.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

bool stringField(const std::string& input, const char* name, std::string* value) {
    std::string marker = std::string("\"") + name + "\":\"";
    std::string::size_type start = input.find(marker);
    std::string::size_type end;
    if (start == std::string::npos) return false;
    start += marker.size();
    end = input.find('"', start);
    if (end == std::string::npos) return false;
    *value = input.substr(start, end - start);
    return true;
}
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    std::string::const_iterator it;
    for (it = value.begin(); it != value.end(); ++it) {
        if (*it == '\\' || *it == '"') {
            result += '\\';
        }
        result += *it;
    }
    return result;
}

bool ensureDirectory(const std::string& directory, std::string* error) {
    if (mkdir(directory.c_str(), 0700) == 0 || errno == EEXIST) {
        return true;
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
    output << "{\n  \"version\": 1,\n  \"devices\": [\n";
    for (i = 0; i < devices.size(); ++i) {
        const Device& device = devices[i];
        output << "    {\"ieee_address\":\"" << jsonEscape(device.ieeeAddress)
               << "\",\"network_address\":" << device.networkAddress << ",\"name\":\"" << jsonEscape(device.name) << "\",\"kind\":\""
               << jsonEscape(device.kind) << "\",\"contact_open\":" << (device.contactOpen ? "true" : "false")
               << ",\"has_contact\":" << (device.hasContact ? "true" : "false")
               << ",\"temperature_celsius\":" << device.temperatureCelsius << ",\"has_temperature\":"
               << (device.hasTemperature ? "true" : "false") << ",\"battery_percent\":" << device.batteryPercent
               << ",\"has_battery\":" << (device.hasBattery ? "true" : "false") << ",\"link_quality\":"
               << device.linkQuality << ",\"last_seen\":" << device.lastSeen << ",\"ias_zone_id\":" << device.iasZoneId
               << ",\"has_low_temperature_alert\":"
               << (device.hasLowTemperatureAlert ? "true" : "false") << ",\"low_temperature_alert\":"
               << device.lowTemperatureAlert << ",\"has_high_temperature_alert\":"
               << (device.hasHighTemperatureAlert ? "true" : "false") << ",\"high_temperature_alert\":"
               << device.highTemperatureAlert << "}";
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
    if (!input) {
        return true;
    }
    /* The state writer emits one device per line. Unknown fields are ignored so
       newer state files remain readable by an older daemon. */
    while (std::getline(input, line)) {
        std::string marker = "\"ieee_address\":\"";
        std::string::size_type start = line.find(marker);
        if (start != std::string::npos) {
            Device device;
            start += marker.size();
            std::string::size_type end = line.find('"', start);
            if (end == std::string::npos) {
                *error = "invalid device state";
                return false;
            }
            device.ieeeAddress = line.substr(start, end - start);
            std::string::size_type networkAddress = line.find("\"network_address\":");
            if (networkAddress != std::string::npos) {
                long parsed = strtol(line.c_str() + networkAddress + 18, 0, 10);
                if (parsed >= 0 && parsed <= 0xffff) device.networkAddress = static_cast<unsigned short>(parsed);
            }
            const char* fields[] = {"name", "kind"};
            std::string* destinations[] = {&device.name, &device.kind};
            int index;
            for (index = 0; index < 2; ++index) {
                std::string field = std::string("\"") + fields[index] + "\":\"";
                std::string::size_type fieldStart = line.find(field);
                if (fieldStart != std::string::npos) {
                    fieldStart += field.size();
                    std::string::size_type fieldEnd = line.find('"', fieldStart);
                    if (fieldEnd != std::string::npos) *destinations[index] = line.substr(fieldStart, fieldEnd - fieldStart);
                }
            }
            device.contactOpen = line.find("\"contact_open\":true") != std::string::npos;
            device.hasContact = line.find("\"has_contact\":true") != std::string::npos;
            device.hasTemperature = line.find("\"has_temperature\":true") != std::string::npos;
            device.hasBattery = line.find("\"has_battery\":true") != std::string::npos;
            device.hasLowTemperatureAlert = line.find("\"has_low_temperature_alert\":true") != std::string::npos;
            device.hasHighTemperatureAlert = line.find("\"has_high_temperature_alert\":true") != std::string::npos;
            std::string::size_type valueStart;
            valueStart = line.find("\"temperature_celsius\":");
            if (valueStart != std::string::npos) device.temperatureCelsius = atof(line.c_str() + valueStart + 22);
            valueStart = line.find("\"battery_percent\":");
            if (valueStart != std::string::npos) device.batteryPercent = atoi(line.c_str() + valueStart + 18);
            valueStart = line.find("\"link_quality\":");
            if (valueStart != std::string::npos) device.linkQuality = atoi(line.c_str() + valueStart + 15);
            valueStart = line.find("\"last_seen\":");
            if (valueStart != std::string::npos) device.lastSeen = atol(line.c_str() + valueStart + 12);
            valueStart = line.find("\"ias_zone_id\":");
            if (valueStart != std::string::npos) {
                long zoneId = strtol(line.c_str() + valueStart + 14, 0, 10);
                if (zoneId >= -1 && zoneId <= 254) device.iasZoneId = static_cast<int>(zoneId);
            }
            valueStart = line.find("\"low_temperature_alert\":");
            if (valueStart != std::string::npos) device.lowTemperatureAlert = atof(line.c_str() + valueStart + 24);
            valueStart = line.find("\"high_temperature_alert\":");
            if (valueStart != std::string::npos) device.highTemperatureAlert = atof(line.c_str() + valueStart + 25);
            devices->push_back(device);
        }
    }
    return true;
}

bool saveNetworkState(const std::string& filename, const NetworkState& network, std::string* error) {
    std::string temporary = filename + ".tmp";
    std::ofstream output(temporary.c_str());
    if (!network.formed || network.extendedPan.size() != 8 || network.networkKey.size() != 16 ||
        network.preconfiguredKey.size() != 16) { *error = "invalid network state"; return false; }
    if (!output) { *error = "cannot open network state for writing"; return false; }
    output << "{\"version\":1,\"formed\":true,\"pan_id\":" << network.panId << "\",\"channel\":"
           << static_cast<unsigned int>(network.channel) << "\",\"extended_pan\":\""
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
    std::string::size_type panAt, channelAt;
    if (!input) return true;
    content.assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (content.find("\"formed\":true") == std::string::npos || !stringField(content, "extended_pan", &extendedPan) ||
        !stringField(content, "network_key", &networkKey) || !stringField(content, "preconfigured_key", &preconfiguredKey) ||
        !hexDecode(extendedPan, &network->extendedPan) || !hexDecode(networkKey, &network->networkKey) ||
        !hexDecode(preconfiguredKey, &network->preconfiguredKey) || network->extendedPan.size() != 8 ||
        network->networkKey.size() != 16 || network->preconfiguredKey.size() != 16) {
        *error = "invalid network state"; return false;
    }
    panAt = content.find("\"pan_id\":"); channelAt = content.find("\"channel\":");
    if (panAt == std::string::npos || channelAt == std::string::npos) { *error = "invalid network state"; return false; }
    network->panId = static_cast<unsigned short>(strtoul(content.c_str() + panAt + 9, 0, 10));
    network->channel = static_cast<unsigned char>(strtoul(content.c_str() + channelAt + 10, 0, 10));
    if (network->channel < 11 || network->channel > 26 || network->panId == 0 || network->panId == 0xffff) {
        *error = "invalid network state"; return false;
    }
    network->formed = true;
    return true;
}
