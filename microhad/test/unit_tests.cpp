#include "ash.h"
#include "gateway.h"
#include "storage.h"
#include "zcl.h"
#include "fcm.h"
#include "notification_worker.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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
    input.networkAddress = 0x1234;
    input.name = "Kitchen window";
    input.kind = "contact";
    input.hasContact = true;
    input.contactOpen = true;
    input.hasTemperature = true;
    input.temperatureCelsius = 19.75;
    input.hasHumidity = true;
    input.humidityPercent = 47.25;
    input.hasOccupancy = true;
    input.occupied = true;
    input.hasBattery = true;
    input.batteryPercent = 76;
    input.linkQuality = 91;
    input.lastSeen = 123456;
    input.iasZoneId = 17;
    devices.push_back(input);
    assert(saveDevices(filename, devices, &error));
    assert(loadDevices(filename, &loaded, &error));
    assert(loaded.size() == 1);
    assert(loaded[0].ieeeAddress == input.ieeeAddress);
    assert(loaded[0].networkAddress == input.networkAddress);
    assert(loaded[0].name == input.name && loaded[0].kind == input.kind);
    assert(loaded[0].hasContact && loaded[0].contactOpen && loaded[0].hasTemperature);
    assert(loaded[0].temperatureCelsius == input.temperatureCelsius);
    assert(loaded[0].hasHumidity && loaded[0].humidityPercent == input.humidityPercent);
    assert(loaded[0].hasOccupancy && loaded[0].occupied);
    assert(loaded[0].hasBattery && loaded[0].batteryPercent == input.batteryPercent);
    assert(loaded[0].linkQuality == input.linkQuality && loaded[0].lastSeen == input.lastSeen);
    assert(loaded[0].iasZoneId == input.iasZoneId);
    unlink(filename.c_str());
    rmdir(directory.c_str());
}

static void testNetworkPersistence() {
    std::ostringstream path;
    std::string error;
    NetworkState input;
    NetworkState loaded;
    path << "/tmp/microhad-zigbee-network-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    std::string filename = directory + "/network.json";
    assert(ensureDirectory(directory, &error));
    input.formed = true; input.panId = 0x1a62; input.channel = 15;
    input.extendedPan.assign(8, 0x12); input.networkKey.assign(16, 0x34); input.preconfiguredKey.assign(16, 0x56);
    assert(saveNetworkState(filename, input, &error));
    {
        std::ifstream serialized(filename.c_str());
        std::string text((std::istreambuf_iterator<char>(serialized)), std::istreambuf_iterator<char>());
        assert(text.find("\"pan_id\":6754,\"channel\":15") != std::string::npos);
    }
    assert(loadNetworkState(filename, &loaded, &error));
    assert(loaded.formed && loaded.panId == input.panId && loaded.channel == input.channel);
    assert(loaded.extendedPan == input.extendedPan && loaded.networkKey == input.networkKey);
    unlink(filename.c_str()); rmdir(directory.c_str());
}

static void testGenericCapabilities() {
    Device device;
    std::vector<unsigned char> temperature;
    std::vector<unsigned char> humidity;
    std::vector<unsigned char> occupancy;
    std::vector<unsigned char> zone;
    std::vector<unsigned char> battery;
    device.ieeeAddress = "0x00124b0000000002";
    temperature.push_back(0x1a); temperature.push_back(0x09); /* 23.30 C */
    assert(applyZclAttribute(&device, 0x0402, 0x0000, temperature, 123));
    assert(device.hasTemperature && device.temperatureCelsius == 23.3);
    humidity.push_back(0xb8); humidity.push_back(0x13); /* 50.48 % */
    assert(applyZclAttribute(&device, 0x0405, 0x0000, humidity, 123));
    assert(device.hasHumidity && device.humidityPercent == 50.48);
    occupancy.push_back(1);
    assert(applyZclAttribute(&device, 0x0406, 0x0000, occupancy, 123));
    assert(device.hasOccupancy && device.occupied);
    {
        std::vector<unsigned char> response;
        response.push_back(0x00); response.push_back(0x00); /* measured value */
        response.push_back(0x00); /* success */
        response.push_back(0x29); /* signed 16-bit */
        response.push_back(0x1a); response.push_back(0x09); /* 23.30 C */
        assert(applyZclReadAttributeResponse(&device, 0x0402, response, 126));
        assert(device.temperatureCelsius == 23.3 && device.lastSeen == 126);
    }
    battery.push_back(170);
    assert(applyZclAttribute(&device, 0x0001, 0x0021, battery, 124));
    assert(device.hasBattery && device.batteryPercent == 85);
    zone.push_back(1); zone.push_back(0);
    assert(applyIasZoneStatus(&device, zone, 125));
    assert(device.hasContact && device.contactOpen && device.lastSeen == 125);
}

static void testJoinStatusJson() {
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    assert(gateway.statusJson().find("\"joining_preparing\":false") != std::string::npos);
}

static void testRamLog() {
    std::string error;
    unlink("/tmp/microhad-zigbee-native.log");
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    assert(!gateway.setJoining(300, &error));
    assert(gateway.logsJson().find("Pairing request rejected: coordinator offline") != std::string::npos);
    unlink("/tmp/microhad-zigbee-native.log");
}

static void testImmediateContactNotification() {
    std::ostringstream path;
    std::string error;
    std::ifstream output;
    std::string content;
    std::vector<unsigned char> zone;
    path << "/tmp/microhad-zigbee-alert-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    Gateway gateway("/dev/null", 115200, directory, "", "", "");
    zone.push_back(1); zone.push_back(0);
    assert(gateway.applyIasZoneStatus("0x00124b0000000003", zone));
    output.open((directory + "/notification-outbox.ndjson").c_str());
    content.assign((std::istreambuf_iterator<char>(output)), std::istreambuf_iterator<char>());
    assert(content.find("Contact opened") != std::string::npos);
    assert(!gateway.sendTestNotification(&error));
    assert(error == "no notification delivery is configured");
    output.close();
    unlink((directory + "/notification-outbox.ndjson").c_str());
    unlink((directory + "/devices.json").c_str());
    rmdir(directory.c_str());
}

static void testNotificationTogglePersistence() {
    std::ostringstream path;
    std::string error;
    path << "/tmp/microhad-zigbee-notification-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    {
        NotificationWorker worker(directory, "", "", "");
        assert(worker.enabled());
        assert(worker.setEnabled(false, &error));
        assert(!worker.enabled());
        assert(!worker.enqueue("Test", "Body", &error));
        assert(error == "notification worker is unavailable" || error == "push notifications are disabled");
    }
    {
        NotificationWorker worker(directory, "", "", "");
        assert(!worker.enabled());
        assert(worker.setEnabled(true, &error));
        assert(worker.enabled());
    }
    unlink((directory + "/notifications.json").c_str());
    rmdir(directory.c_str());
}

static void testFcmMessageConstruction() {
    FirebaseServiceAccount account;
    std::string error;
    std::string json = "{\"project_id\":\"project\",\"client_email\":\"x@example.test\",\"private_key\":\"-----BEGIN\\nKEY-----\\n\"}";
    assert(parseFirebaseServiceAccount(json, &account, &error));
    assert(account.privateKeyPem == "-----BEGIN\nKEY-----\n");
    assert(base64UrlEncode("foo") == "Zm9v");
    assert(base64UrlEncode("f") == "Zg");
    std::string decoded;
    assert(base64UrlDecode("Zm9v", &decoded) && decoded == "foo");
    assert(base64UrlDecode("Zg", &decoded) && decoded == "f");
    assert(makeFcmMessage("zigbee_alerts", "Window open", "Kitchen").find("\"topic\":\"zigbee_alerts\"") != std::string::npos);
}

int main() {
    testAshRoundTrip();
    testAshRejectsBadCrc();
    testDevicePersistence();
    testNetworkPersistence();
    testGenericCapabilities();
    testJoinStatusJson();
    testRamLog();
    testImmediateContactNotification();
    testNotificationTogglePersistence();
    testFcmMessageConstruction();
    printf("unit tests passed\n");
    return 0;
}
