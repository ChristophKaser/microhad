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
               << "\",\"name\":\"" << jsonEscape(device.name) << "\",\"kind\":\""
               << jsonEscape(device.kind) << "\",\"contact_open\":" << (device.contactOpen ? "true" : "false")
               << ",\"has_contact\":" << (device.hasContact ? "true" : "false")
               << ",\"temperature_celsius\":" << device.temperatureCelsius << ",\"has_temperature\":"
               << (device.hasTemperature ? "true" : "false") << ",\"battery_percent\":" << device.batteryPercent
               << ",\"has_battery\":" << (device.hasBattery ? "true" : "false") << ",\"link_quality\":"
               << device.linkQuality << ",\"last_seen\":" << device.lastSeen << ",\"has_low_temperature_alert\":"
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
            valueStart = line.find("\"low_temperature_alert\":");
            if (valueStart != std::string::npos) device.lowTemperatureAlert = atof(line.c_str() + valueStart + 24);
            valueStart = line.find("\"high_temperature_alert\":");
            if (valueStart != std::string::npos) device.highTemperatureAlert = atof(line.c_str() + valueStart + 25);
            devices->push_back(device);
        }
    }
    return true;
}
