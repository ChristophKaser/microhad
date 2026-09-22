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
#include <fcntl.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

class GatewayTestPeer {
public:
    static void reviewAndTransportRegressions() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        gateway.serialFd_ = open("/dev/null", O_WRONLY);
        assert(gateway.serialFd_ >= 0);
        gateway.sendEzspCommand(5, std::vector<unsigned char>());
        std::vector<unsigned char> original = gateway.ashPendingData_;
        gateway.sendEzspCommand(5, std::vector<unsigned char>());
        assert(gateway.ashTransmitQueue_.size() == 1);
        gateway.handleAshFrame(std::vector<unsigned char>(1, 0xa0));
        assert(gateway.ashRetries_ == 1 && gateway.ashPendingData_ == original);
        assert(gateway.ashSendSequence_ == 0);
        gateway.handleAshFrame(std::vector<unsigned char>(1, 0x81));
        gateway.pollAshTransmit();
        assert(gateway.ashSendSequence_ == 1 && gateway.ashTransmitQueue_.empty());
        gateway.ashAckDeadline_ = 0;
        gateway.pollAshTransmit();
        assert(gateway.ashRetries_ == 1);
        gateway.handleAshFrame(std::vector<unsigned char>(1, 0x10)); /* gap */
        assert(gateway.ashReceiveSequence_ == 0);
        gateway.handleAshFrame(std::vector<unsigned char>(1, 0x00));
        assert(gateway.ashReceiveSequence_ == 1);
        gateway.handleAshFrame(std::vector<unsigned char>(1, 0x08)); /* duplicate */
        assert(gateway.ashReceiveSequence_ == 1);

        const unsigned short node = 0x4321;
        Gateway::InterviewState state;
        state.ieeeAddress = "0x00124b00000000ab";
        state.operations.push_back(Gateway::InterviewOperation());
        gateway.interviews_[node] = state;
        std::vector<unsigned char> response(21, 0);
        response[3] = 2; response[4] = 0x80;
        response[14] = 0x21; response[15] = 0x43;
        response[18] = 2; /* seq 0, status success */
        gateway.handleIncomingMessage(response);
        assert(gateway.interviews_[node].operations[0].type == Gateway::InterviewNodeDescriptor);
        gateway.interviews_[node].operations[0].transactionSequenceAssigned = true;
        gateway.interviews_[node].paused = true;
        gateway.handleIncomingMessage(response);
        assert(gateway.interviews_[node].operations[0].type == Gateway::InterviewActiveEndpoints);
        assert(!gateway.interviews_[node].paused);

        const Gateway::InterviewOperationType types[] = {
            Gateway::InterviewActiveEndpoints, Gateway::InterviewSimpleDescriptor,
            Gateway::InterviewBind, Gateway::InterviewBasicRead,
            Gateway::InterviewAttributeRead, Gateway::InterviewConfigureReporting};
        const unsigned short clusters[] = {0x8005, 0x8004, 0x8021, 0, 0x0402, 0x0402};
        gateway.nodeAddresses_[node] = state.ieeeAddress;
        for (unsigned int i = 0; i < 6; ++i) {
            state.operations[0] = Gateway::InterviewOperation();
            state.operations[0].type = types[i];
            state.operations[0].cluster = clusters[i];
            gateway.interviews_[node] = state;
            response.assign(32, 0);
            response[3] = static_cast<unsigned char>(clusters[i]);
            response[4] = static_cast<unsigned char>(clusters[i] >> 8);
            response[14] = 0x21; response[15] = 0x43;
            response[18] = 13;
            if (i == 1) response[23] = 8; /* valid empty simple descriptor */
            if (i >= 3) { response[19] = 0x18; response[21] = i == 5 ? 7 : 1; }
            gateway.handleIncomingMessage(response);
            assert(gateway.interviews_.count(node) == 1);
            assert(gateway.interviews_[node].operations[0].type == types[i]);
        }

        Gateway::InterviewOperation& operation = gateway.interviews_[node].operations[0];
        operation.attempts = 1;
        gateway.failInterviewOperation(node, "buffer full", true);
        assert(!gateway.interviews_[node].paused && operation.retryAt > time(0));
        operation.attempts = 3;
        gateway.failInterviewOperation(node, "buffer full", true);
        assert(gateway.interviews_[node].paused);

        /* An unrelated sendUnicast response must not reject this interview. */
        gateway.ezspVersion_ = 8;
        gateway.interviewRequestInFlight_ = true;
        gateway.interviewSendAccepted_ = false;
        gateway.interviewNode_ = node;
        gateway.interviewEzspSequence_ = 42;
        operation.attempts = 1;
        gateway.interviews_[node].paused = false;
        const unsigned char rejected[] = {41, 0x80, 1, 0x34, 0, 0x18};
        std::vector<unsigned char> reply(rejected, rejected + sizeof(rejected));
        gateway.handleEzspFrame(reply);
        assert(gateway.interviewRequestInFlight_);
        reply[0] = 42;
        gateway.handleEzspFrame(reply);
        assert(!gateway.interviewRequestInFlight_ && !gateway.interviews_[node].paused);

        /* Decode the outbound enrollment reply and check reversed endpoints. */
        gateway.ashPendingData_.clear();
        gateway.ashTransmitQueue_.clear();
        gateway.devices_[state.ieeeAddress].iasZoneId = 4;
        response.assign(26, 0);
        response[1] = 4; response[2] = 1;
        response[3] = 0; response[4] = 5;
        response[5] = 3; response[6] = 1;
        response[14] = 0x21; response[15] = 0x43;
        response[18] = 7;
        response[19] = 0x19; response[20] = 9; response[21] = 1;
        gateway.handleIncomingMessage(response);
        std::vector<unsigned char> command = gateway.ashPendingData_;
        unsigned char random = 0x42;
        for (unsigned long i = 0; i < command.size(); ++i) {
            command[i] ^= random;
            random = (random & 1) ? static_cast<unsigned char>((random >> 1) ^ 0xb8) :
                                   static_cast<unsigned char>(random >> 1);
        }
        assert(command.size() >= 14 && command[12] == 1 && command[13] == 3);
    }
    static bool parseSimpleDescriptor(const Gateway& gateway, const std::vector<unsigned char>& message,
                                      EndpointDescriptor* descriptor) {
        return gateway.parseSimpleDescriptor(message, descriptor);
    }
    static void buildConfiguration(Gateway* gateway, unsigned short node, const std::string& ieee,
                                   const EndpointDescriptor& endpoint) {
        Gateway::InterviewState state;
        state.ieeeAddress = ieee;
        gateway->interviews_[node] = state;
        gateway->devices_[ieee].ieeeAddress = ieee;
        gateway->devices_[ieee].endpoints.push_back(endpoint);
        gateway->buildConfigurationOperations(node);
    }
    static unsigned long operationCount(const Gateway& gateway, unsigned short node) {
        return gateway.interviews_.find(node)->second.operations.size();
    }
    static int operationType(const Gateway& gateway, unsigned short node, unsigned long index) {
        return static_cast<int>(gateway.interviews_.find(node)->second.operations[index].type);
    }
    static unsigned short operationCluster(const Gateway& gateway, unsigned short node, unsigned long index) {
        return gateway.interviews_.find(node)->second.operations[index].cluster;
    }
    static bool pauseAndWake(Gateway* gateway, unsigned short node) {
        Gateway::InterviewState state;
        Gateway::InterviewOperation operation;
        state.ieeeAddress = "0x00124b0000000011";
        operation.attempts = 2;
        state.operations.push_back(operation);
        gateway->interviews_[node] = state;
        gateway->interviewRequestInFlight_ = true;
        gateway->interviewNode_ = node;
        gateway->failInterviewOperation(node, "test failure");
        if (!gateway->interviews_[node].paused) return false;
        gateway->wakeInterview(node);
        return !gateway->interviews_[node].paused;
    }
    static bool retryKeepsTransactionSequence(Gateway* gateway, unsigned short node) {
        Gateway::InterviewState state;
        Gateway::InterviewOperation operation;
        state.ieeeAddress = "0x00124b0000000012";
        state.operations.push_back(operation);
        gateway->interviews_[node] = state;
        gateway->sendInterviewOperation(node, &gateway->interviews_[node].operations[0]);
        unsigned char sequence = gateway->interviews_[node].operations[0].transactionSequence;
        gateway->failInterviewOperation(node, "test timeout");
        gateway->sendInterviewOperation(node, &gateway->interviews_[node].operations[0]);
        return gateway->interviews_[node].operations[0].transactionSequence == sequence;
    }
    static bool migrateRejoinedInterview(Gateway* gateway, unsigned short oldNode, unsigned short newNode) {
        Gateway::InterviewState state;
        Gateway::InterviewOperation operation;
        const unsigned char eui64[] = {0x13, 0x00, 0x00, 0x00, 0x00, 0x4b, 0x12, 0x00};
        state.ieeeAddress = "0x00124b0000000013";
        state.operations.push_back(operation);
        gateway->interviews_[oldNode] = state;
        gateway->nodeAddresses_[oldNode] = state.ieeeAddress;
        gateway->interviewRequestInFlight_ = true;
        gateway->interviewNode_ = newNode;
        gateway->interviews_[newNode] = Gateway::InterviewState();
        gateway->nodeAddresses_[newNode] = "0x00124b00000000ff";
        gateway->registerNode(newNode, eui64);
        return gateway->nodeAddresses_.find(oldNode) == gateway->nodeAddresses_.end() &&
               gateway->nodeAddresses_[newNode] == state.ieeeAddress &&
               gateway->interviews_.find(oldNode) == gateway->interviews_.end() &&
               gateway->interviews_.find(newNode) != gateway->interviews_.end() &&
               !gateway->interviewRequestInFlight_;
    }
    static bool contactCloseDuringInterview(Gateway* gateway) {
        const unsigned short contactNode = 0x1234;
        const std::string ieee = "0x00124b0000000014";
        std::vector<unsigned char> payload(19, 0);
        Gateway::InterviewState interview;
        Gateway::InterviewOperation operation;
        interview.ieeeAddress = "0x00124b0000000015";
        interview.operations.push_back(operation);
        gateway->interviews_[0x5678] = interview;
        gateway->interviewRequestInFlight_ = true;
        gateway->interviewNode_ = 0x5678;
        gateway->nodeAddresses_[contactNode] = ieee;
        gateway->devices_[ieee].ieeeAddress = ieee;
        gateway->devices_[ieee].hasContact = true;
        gateway->devices_[ieee].contactOpen = true;
        payload[1] = 0x04; payload[2] = 0x01; /* Home Automation profile */
        payload[3] = 0x00; payload[4] = 0x05; /* IAS Zone cluster */
        payload[12] = 99;
        payload[14] = 0x34; payload[15] = 0x12;
        payload[18] = 5;
        payload.push_back(0x19); payload.push_back(7); payload.push_back(0x00);
        payload.push_back(0x00); payload.push_back(0x00);
        gateway->handleIncomingMessage(payload);
        return gateway->devices_[ieee].hasContact && !gateway->devices_[ieee].contactOpen &&
               gateway->devices_[ieee].linkQuality == 99;
    }
    static bool frameLogsContainDecodedDetails() {
        const unsigned short node = 0x1234;
        const std::string ieee = "0x00124b0000000020";
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        std::vector<unsigned char> request;
        std::vector<unsigned char> incoming(24, 0);
        gateway.logs_.clear();
        gateway.nodeAddresses_[node] = ieee;
        gateway.devices_[ieee].ieeeAddress = ieee;
        gateway.devices_[ieee].name = "Kitchen sensor";
        request.push_back(0x10); request.push_back(7); request.push_back(0x00); request.push_back(0x00); request.push_back(0x00);
        gateway.sendApsUnicast(node, 0x0104, 0x0402, 1, 1, request, 9);
        if (gateway.logsJson().find("FRAME TX recipient=Kitchen sensor ieee=0x00124b0000000020") == std::string::npos ||
            gateway.logsJson().find("Read Attributes") == std::string::npos ||
            gateway.logsJson().find("payload=1007000000") == std::string::npos) return false;
        incoming[1] = 0x04; incoming[2] = 0x01; incoming[3] = 0x02; incoming[4] = 0x04;
        incoming[5] = 1; incoming[6] = 1; incoming[11] = 44; incoming[12] = 88; incoming[13] = 0xd8;
        incoming[14] = 0x34; incoming[15] = 0x12; incoming[18] = 5;
        incoming[19] = 0x18; incoming[20] = 8; incoming[21] = 0x0a; incoming[22] = 0; incoming[23] = 0;
        gateway.handleIncomingMessage(incoming);
        return gateway.logsJson().find("FRAME RX sender=Kitchen sensor ieee=0x00124b0000000020") != std::string::npos &&
               gateway.logsJson().find("lqi=88 rssi=-40") != std::string::npos &&
               gateway.logsJson().find("Report Attributes") != std::string::npos;
    }
};

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
    /* A CANCEL after a complete frame must only discard a partial frame. */
    encoded.push_back(0x1a);
    transport.feed(&encoded[0], encoded.size());
    assert(transport.popFrame(&frame));
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
    EndpointDescriptor endpoint;
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
    input.contactInitialAlertMinutes = 20;
    input.contactReminderAlertMinutes = 7;
    input.sensorRefreshIntervalMinutes = 17;
    input.manufacturerName = "eWeLink";
    input.modelId = "TH01";
    endpoint.endpoint = 1;
    endpoint.profileId = 0x0104;
    endpoint.deviceId = 0x0302;
    endpoint.deviceVersion = 1;
    endpoint.inputClusters.push_back(0x0000);
    endpoint.inputClusters.push_back(0x0001);
    endpoint.inputClusters.push_back(0x0402);
    endpoint.inputClusters.push_back(0x0405);
    endpoint.outputClusters.push_back(0x0003);
    input.endpoints.push_back(endpoint);
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
    assert(loaded[0].contactInitialAlertMinutes == 20 && loaded[0].contactReminderAlertMinutes == 7);
    assert(loaded[0].sensorRefreshIntervalMinutes == 17);
    assert(loaded[0].manufacturerName == "eWeLink" && loaded[0].modelId == "TH01");
    assert(loaded[0].endpoints.size() == 1);
    assert(loaded[0].endpoints[0].endpoint == 1 && loaded[0].endpoints[0].profileId == 0x0104);
    assert(loaded[0].endpoints[0].deviceId == 0x0302 && loaded[0].endpoints[0].deviceVersion == 1);
    assert(loaded[0].endpoints[0].inputClusters.size() == 4);
    assert(loaded[0].endpoints[0].inputClusters[2] == 0x0402 && loaded[0].endpoints[0].outputClusters[0] == 0x0003);
    unlink(filename.c_str());
    rmdir(directory.c_str());
}

static void testDeviceSettings() {
    std::ostringstream path;
    std::string error;
    std::vector<unsigned char> zone;
    std::vector<unsigned char> temperature;
    path << "/tmp/microhad-zigbee-settings-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    Gateway gateway("/dev/null", 115200, directory, "", "", "");
    zone.push_back(1); zone.push_back(0);
    temperature.push_back(0x1a); temperature.push_back(0x09);
    assert(gateway.applyIasZoneStatus("0x00124b0000000004", zone));
    assert(gateway.applyAttributeReport("0x00124b0000000004", 0x0402, 0x0000, temperature));
    assert(gateway.setDeviceSettings("0x00124b0000000004", "Office window", true, 5, true, 30,
                                     true, 20, 7, true, 17, &error));
    assert(gateway.statusJson().find("\"contact_initial_alert_minutes\":20") != std::string::npos);
    assert(gateway.statusJson().find("\"contact_reminder_alert_minutes\":7") != std::string::npos);
    assert(gateway.statusJson().find("\"sensor_refresh_interval_minutes\":17") != std::string::npos);
    assert(gateway.setDeviceSettings("0x00124b0000000004", "Office window", true, 5, true, 30,
                                     true, 20, 7, true, 0, &error));
    assert(gateway.statusJson().find("\"sensor_refresh_interval_minutes\":0") != std::string::npos);
    assert(!gateway.setDeviceSettings("0x00124b0000000004", "Office window", true, 30, true, 5,
                                      true, 20, 7, true, 17, &error));
    assert(error == "invalid temperature thresholds");
    assert(!gateway.setDeviceSettings("0x00124b0000000004", "Office window", false, 0, false, 0,
                                      true, 0, 7, true, 0, &error));
    assert(error == "invalid contact notification times");
    unlink((directory + "/devices.json").c_str());
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
    std::vector<unsigned char> xiaomi;
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
    xiaomi.push_back(8); /* length */
    xiaomi.push_back(0x64); xiaomi.push_back(0x29); xiaomi.push_back(0xa8); xiaomi.push_back(0x08); /* 22.16 C */
    xiaomi.push_back(0x65); xiaomi.push_back(0x21); xiaomi.push_back(0xb8); xiaomi.push_back(0x13); /* 50.48 % */
    device = Device();
    assert(applyZclAttribute(&device, 0x0000, 0xff01, xiaomi, 124));
    assert(device.hasTemperature && device.temperatureCelsius == 22.16);
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

static void testConfigureReportingEncoding() {
    std::vector<unsigned char> battery = buildZclConfigureReporting(7, 0x0021, 0x20, 3600, 65000, 1);
    std::vector<unsigned char> temperature = buildZclConfigureReporting(8, 0x0000, 0x29, 30, 3600, 20);
    assert(battery.size() == 12);
    assert(battery[1] == 7 && battery[6] == 0x20 && battery[11] == 1);
    assert(temperature.size() == 13);
    assert(temperature[1] == 8 && temperature[6] == 0x29 && temperature[11] == 20 && temperature[12] == 0);
}

static void testInterviewDescriptorAndPlanning() {
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    std::vector<unsigned char> response;
    EndpointDescriptor descriptor;
    response.push_back(1); response.push_back(0); response.push_back(0x34); response.push_back(0x12);
    response.push_back(18); response.push_back(1); response.push_back(0x04); response.push_back(0x01);
    response.push_back(0x02); response.push_back(0x03); response.push_back(1); response.push_back(4);
    response.push_back(0x00); response.push_back(0x00); response.push_back(0x01); response.push_back(0x00);
    response.push_back(0x02); response.push_back(0x04); response.push_back(0x05); response.push_back(0x04);
    response.push_back(1); response.push_back(0x03); response.push_back(0x00);
    assert(GatewayTestPeer::parseSimpleDescriptor(gateway, response, &descriptor));
    assert(descriptor.endpoint == 1 && descriptor.profileId == 0x0104 && descriptor.deviceId == 0x0302);
    assert(descriptor.inputClusters.size() == 4 && descriptor.inputClusters[2] == 0x0402);
    GatewayTestPeer::buildConfiguration(&gateway, 0x1234, "0x00124b0000000010", descriptor);
    assert(GatewayTestPeer::operationCount(gateway, 0x1234) == 10);
    assert(GatewayTestPeer::operationType(gateway, 0x1234, 0) == 4); /* InterviewBind */
    assert(GatewayTestPeer::operationCluster(gateway, 0x1234, 0) == 0x0402);
    assert(GatewayTestPeer::operationCluster(gateway, 0x1234, 1) == 0x0405);
    assert(GatewayTestPeer::operationCluster(gateway, 0x1234, 2) == 0x0001);
    assert(GatewayTestPeer::pauseAndWake(&gateway, 0x1235));
    assert(GatewayTestPeer::retryKeepsTransactionSequence(&gateway, 0x1236));
    assert(GatewayTestPeer::contactCloseDuringInterview(&gateway));
    {
        std::ostringstream path;
        std::string error;
        path << "/tmp/microhad-zigbee-rejoin-test-" << static_cast<long>(getpid());
        assert(ensureDirectory(path.str(), &error));
        Gateway rejoinGateway("/dev/null", 115200, path.str(), "", "", "");
        assert(GatewayTestPeer::migrateRejoinedInterview(&rejoinGateway, 0x1237, 0x1238));
        unlink((path.str() + "/devices.json").c_str());
        unlink((path.str() + "/devices-state.json").c_str());
        rmdir(path.str().c_str());
    }
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

static void testFrameLogs() {
    unlink("/tmp/microhad-zigbee-native.log");
    assert(GatewayTestPeer::frameLogsContainDecodedDetails());
    unlink("/tmp/microhad-zigbee-native.log");
}

static void testContactNotificationIsDeferred() {
    std::ostringstream path;
    std::string error;
    std::vector<unsigned char> zone;
    path << "/tmp/microhad-zigbee-alert-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    NotificationWorker worker(directory, "", "", "");
    assert(worker.setEnabled(false, &error));
    Gateway gateway("/dev/null", 115200, directory, "", "", "", &worker);
    zone.push_back(1); zone.push_back(0);
    assert(gateway.applyIasZoneStatus("0x00124b0000000003", zone));
    assert(gateway.logsJson().find("Contact opened") == std::string::npos);
    assert(!gateway.sendTestNotification(&error));
    assert(error == "push notifications are disabled");
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
    GatewayTestPeer::reviewAndTransportRegressions();
    testAshRoundTrip();
    testAshRejectsBadCrc();
    testDevicePersistence();
    testDeviceSettings();
    testNetworkPersistence();
    testGenericCapabilities();
    testConfigureReportingEncoding();
    testInterviewDescriptorAndPlanning();
    testJoinStatusJson();
    testRamLog();
    testFrameLogs();
    testContactNotificationIsDeferred();
    testNotificationTogglePersistence();
    testFcmMessageConstruction();
    printf("unit tests passed\n");
    return 0;
}
