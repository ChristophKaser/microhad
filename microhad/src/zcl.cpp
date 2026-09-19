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
