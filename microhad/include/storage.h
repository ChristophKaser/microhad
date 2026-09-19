#ifndef MICROHAD_ZIGBEE_STORAGE_H
#define MICROHAD_ZIGBEE_STORAGE_H

#include <string>
#include <vector>

struct Device;

bool ensureDirectory(const std::string& directory, std::string* error);
bool loadDevices(const std::string& filename, std::vector<Device>* devices, std::string* error);
bool saveDevices(const std::string& filename, const std::vector<Device>& devices, std::string* error);
std::string jsonEscape(const std::string& value);

#endif
