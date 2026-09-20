#include "zcl.h"
#include "gateway.h"

namespace {
unsigned short littleEndian16(const std::vector<unsigned char>& value) {
    return static_cast<unsigned short>(value[0]) | (static_cast<unsigned short>(value[1]) << 8);
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
        device->kind = "humidity";
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
    } else {
        return false;
    }
    device->lastSeen = observedAt;
    return true;
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
        if (type == 0x08 || type == 0x10 || type == 0x18 || type == 0x20 || type == 0x28 || type == 0x30) length = 1;
        else if (type == 0x21 || type == 0x29 || type == 0x31) length = 2;
        else if (type == 0x23 || type == 0x2b || type == 0x39) length = 4;
        else break;
        if (offset + 4 + length > payload.size()) break;
        value.assign(payload.begin() + offset + 4, payload.begin() + offset + 4 + length);
        if (applyZclAttribute(device, cluster, attribute, value, observedAt)) changed = true;
        offset += 4 + length;
    }
    return changed;
}
