#include "ash.h"
#include "gateway.h"
#include "storage.h"
#include "zcl.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

static void testAshRoundTrip() {
    std::vector<unsigned char> payload;
    std::vector<unsigned char> encoded;
    std::vector<unsigned char> frame;
    AshTransport transport;
    payload.push_back(0x7e);
    payload.push_back(0x11);
    payload.push_back(0x12);
    encoded = AshTransport::encodeData(0x00, payload);
    transport.feed(&encoded[0], encoded.size());
    assert(transport.popFrame(&frame));
    assert(frame.size() == 4);
    assert(frame[0] == 0x00 && frame[1] == 0x7e && frame[2] == 0x11 && frame[3] == 0x12);
}

static void testAshRejectsBadCrc() {
    std::vector<unsigned char> payload;
    std::vector<unsigned char> encoded;
    std::vector<unsigned char> frame;
    AshTransport transport;
    payload.push_back(0x44);
    encoded = AshTransport::encodeData(0x00, payload);
    encoded[1] ^= 0x01;
    transport.feed(&encoded[0], encoded.size());
    assert(!transport.popFrame(&frame));
}

static void testDevicePersistence() {
    std::ostringstream path;
    path << "/tmp/microhad-zigbee-native-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    std::string filename = directory + "/devices.json";
    std::string error;
    Device input;
    std::vector<Device> devices;
    std::vector<Device> loaded;
    assert(ensureDirectory(directory, &error));
    input.ieeeAddress = "0x00124b0000000001";
    input.name = "Kitchen window";
    devices.push_back(input);
    assert(saveDevices(filename, devices, &error));
    assert(loadDevices(filename, &loaded, &error));
    assert(loaded.size() == 1);
    assert(loaded[0].ieeeAddress == input.ieeeAddress);
    unlink(filename.c_str());
    rmdir(directory.c_str());
}

static void testGenericCapabilities() {
    Device device;
    std::vector<unsigned char> temperature;
    std::vector<unsigned char> zone;
    std::vector<unsigned char> battery;
    device.ieeeAddress = "0x00124b0000000002";
    temperature.push_back(0x1a); temperature.push_back(0x09); /* 23.30 C */
    assert(applyZclAttribute(&device, 0x0402, 0x0000, temperature, 123));
    assert(device.hasTemperature && device.temperatureCelsius == 23.3);
    battery.push_back(170);
    assert(applyZclAttribute(&device, 0x0001, 0x0021, battery, 124));
    assert(device.hasBattery && device.batteryPercent == 85);
    zone.push_back(1); zone.push_back(0);
    assert(applyIasZoneStatus(&device, zone, 125));
    assert(device.hasContact && device.contactOpen && device.lastSeen == 125);
}

int main() {
    testAshRoundTrip();
    testAshRejectsBadCrc();
    testDevicePersistence();
    testGenericCapabilities();
    printf("unit tests passed\n");
    return 0;
}
