#include "zcl.h"
#include "gateway.h"

namespace {
unsigned short littleEndian16(const std::vector<unsigned char>& value) {
    return static_cast<unsigned short>(value[0]) | (static_cast<unsigned short>(value[1]) << 8);
}

unsigned long valueLength(unsigned char type, const std::vector<unsigned char>& value, unsigned long offset) {
    if (type == 0x08 || type == 0x10 || type == 0x18 || type == 0x20 || type == 0x28 || type == 0x30) return 1;
    if (type == 0x21 || type == 0x29 || type == 0x31) return 2;
    if (type == 0x23 || type == 0x2b || type == 0x39) return 4;
    if ((type == 0x41 || type == 0x42) && offset < value.size()) return 1 + value[offset];
    if ((type == 0x43 || type == 0x44) && offset + 1 < value.size())
        return 2 + static_cast<unsigned long>(value[offset]) + (static_cast<unsigned long>(value[offset + 1]) << 8);
    return 0;
}

bool applyXiaomiMeasurementData(Device* device, const std::vector<unsigned char>& value, long observedAt) {
    unsigned long offset = 0;
    bool changed = false;
    /* Xiaomi/Lumi puts a length-prefixed TLV stream in Basic attribute 0xff01. */
    if (!value.empty() && static_cast<unsigned long>(value[0]) + 1 == value.size()) offset = 1;
    while (offset + 2 <= value.size()) {
        unsigned char identifier = value[offset++];
        unsigned char type = value[offset++];
        unsigned long length = valueLength(type, value, offset);
        if (length == 0 || offset + length > value.size()) break;
        std::vector<unsigned char> field(value.begin() + offset, value.begin() + offset + length);
        if (identifier == 0x64 && type == 0x29 && field.size() == 2) {
            device->hasTemperature = true;
            device->temperatureCelsius = static_cast<double>(static_cast<short>(littleEndian16(field))) / 100.0;
            device->kind = device->hasHumidity ? "temperature/humidity" : "temperature";
            changed = true;
        } else if (identifier == 0x65 && type == 0x21 && field.size() == 2) {
            device->hasHumidity = true;
            device->humidityPercent = static_cast<double>(littleEndian16(field)) / 100.0;
            device->kind = device->hasTemperature ? "temperature/humidity" : "humidity";
            changed = true;
        }
        offset += length;
    }
    if (changed) device->lastSeen = observedAt;
    return changed;
}
}

bool applyZclAttribute(Device* device, unsigned short cluster, unsigned short attribute,
                       const std::vector<unsigned char>& value, long observedAt) {
    if (cluster == 0x0402 && attribute == 0x0000 && value.size() >= 2) {
        short raw = static_cast<short>(littleEndian16(value));
        device->hasTemperature = true;
        device->temperatureCelsius = static_cast<double>(raw) / 100.0;
        device->kind = "temperature";
    } else if (cluster == 0x0405 && attribute == 0x0000 && value.size() >= 2) {
        device->hasHumidity = true;
        device->humidityPercent = static_cast<double>(littleEndian16(value)) / 100.0;
        device->kind = device->hasTemperature ? "temperature/humidity" : "humidity";
    } else if (cluster == 0x0406 && attribute == 0x0000 && value.size() >= 1) {
        device->hasOccupancy = true;
        device->occupied = (value[0] & 0x01) != 0;
        device->kind = "occupancy";
    } else if (cluster == 0x0001 && attribute == 0x0021 && value.size() >= 1) {
        device->hasBattery = true;
        /* Power Configuration reports half-percent units. */
        device->batteryPercent = static_cast<int>(value[0]) / 2;
    } else if (cluster == 0x0500 && attribute == 0x0002 && value.size() >= 2) {
        device->hasContact = true;
        device->contactOpen = (littleEndian16(value) & 0x0001) != 0;
        device->kind = "contact";
    } else if (cluster == 0x0006 && attribute == 0x0000 && value.size() >= 1) {
        device->hasLight = true;
        device->lightOn = value[0] != 0;
        if (device->kind == "generic") device->kind = "light";
    } else if (cluster == 0x0008 && attribute == 0x0000 && value.size() >= 1) {
        device->hasLight = true;
        device->hasBrightness = true;
        device->brightnessPercent = (static_cast<int>(value[0]) * 100 + 127) / 254;
        if (device->kind == "generic") device->kind = "dimmable light";
    } else if (cluster == 0x0000 && attribute == 0xff01) {
        return applyXiaomiMeasurementData(device, value, observedAt);
    } else {
        return false;
    }
    device->lastSeen = observedAt;
    return true;
}

bool isLightAttribute(unsigned short cluster, unsigned short attribute) {
    return (cluster == 0x0006 || cluster == 0x0008) && attribute == 0x0000;
}

bool applyIasZoneStatus(Device* device, const std::vector<unsigned char>& payload, long observedAt) {
    if (payload.size() < 2) return false;
    device->hasContact = true;
    device->contactOpen = (littleEndian16(payload) & 0x0001) != 0;
    device->kind = "contact";
    device->lastSeen = observedAt;
    return true;
}

bool applyZclReadAttributeResponse(Device* device, unsigned short cluster,
                                   const std::vector<unsigned char>& payload, long observedAt) {
    unsigned long offset = 0;
    bool changed = false;
    while (offset + 3 <= payload.size()) {
        unsigned short attribute = static_cast<unsigned short>(payload[offset]) |
                                   (static_cast<unsigned short>(payload[offset + 1]) << 8);
        unsigned char status = payload[offset + 2];
        unsigned char type;
        unsigned long length;
        std::vector<unsigned char> value;
        if (status != 0) { offset += 3; continue; }
        if (offset + 4 > payload.size()) break;
        type = payload[offset + 3];
        length = valueLength(type, payload, offset + 4);
        if (length == 0) break;
        if (offset + 4 + length > payload.size()) break;
        value.assign(payload.begin() + offset + 4, payload.begin() + offset + 4 + length);
        if (applyZclAttribute(device, cluster, attribute, value, observedAt)) changed = true;
        offset += 4 + length;
    }
    return changed;
}

std::vector<unsigned char> buildZclConfigureReporting(unsigned char transactionSequence, unsigned short attribute,
                                                       unsigned char dataType, unsigned short minimumInterval,
                                                       unsigned short maximumInterval, unsigned short reportableChange) {
    std::vector<unsigned char> request;
    request.push_back(0x10);
    request.push_back(transactionSequence);
    request.push_back(0x06);
    request.push_back(0x00);
    request.push_back(static_cast<unsigned char>(attribute));
    request.push_back(static_cast<unsigned char>(attribute >> 8));
    request.push_back(dataType);
    request.push_back(static_cast<unsigned char>(minimumInterval));
    request.push_back(static_cast<unsigned char>(minimumInterval >> 8));
    request.push_back(static_cast<unsigned char>(maximumInterval));
    request.push_back(static_cast<unsigned char>(maximumInterval >> 8));
    request.push_back(static_cast<unsigned char>(reportableChange));
    if (dataType == 0x21 || dataType == 0x29 || dataType == 0x31)
        request.push_back(static_cast<unsigned char>(reportableChange >> 8));
    return request;
}
