#include "ash.h"
#include "gateway.h"
#include "storage.h"

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

int main() {
    testAshRoundTrip();
    testAshRejectsBadCrc();
    testDevicePersistence();
    printf("unit tests passed\n");
    return 0;
}
