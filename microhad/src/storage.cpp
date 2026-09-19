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
               << device.linkQuality << ",\"last_seen\":" << device.lastSeen << "}";
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
    /* State is owned by this daemon. Deliberately tolerate unknown/new fields;
       a complete JSON parser is introduced with notification configuration. */
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
            devices->push_back(device);
        }
    }
    return true;
}
