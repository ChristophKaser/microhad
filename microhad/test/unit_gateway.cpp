#include "ash.h"
#include "gateway.h"
#include "storage.h"
#include "zcl.h"
#include "fcm.h"
#include "notification_worker.h"
#include "json_util.h"
#include "web_push.h"

#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

class GatewayTestPeer {
public:
    static bool contactAlertDeadlines(Gateway* gateway, const std::string& ieeeAddress) {
        std::vector<unsigned char> open(2, 0);
        open[0] = 1;
        if (!gateway->applyIasZoneStatus(ieeeAddress, open)) return false;
        long firstDeadline = gateway->nextContactAlertAt_[ieeeAddress];
        if (!gateway->applyIasZoneStatus(ieeeAddress, open) || gateway->nextContactAlertAt_[ieeeAddress] != firstDeadline)
            return false;
        gateway->nextContactAlertAt_[ieeeAddress] = time(0) - 1;
        gateway->poll();
        long reminderDeadline = gateway->nextContactAlertAt_[ieeeAddress];
        if (reminderDeadline <= time(0)) return false;
        gateway->nextContactAlertAt_[ieeeAddress] = time(0) - 1;
        gateway->poll();
        return true;
    }
    static unsigned long logOccurrences(const Gateway& gateway, const std::string& text) {
        std::string logs = gateway.logsJson();
        std::string::size_type at = 0;
        unsigned long count = 0;
        while ((at = logs.find(text, at)) != std::string::npos) { ++count; at += text.size(); }
        return count;
    }
    static void completeViewSchema(Gateway* gateway) {
        Device device;
        EndpointDescriptor endpoint;
        device.ieeeAddress = "0xview";
        device.name = "Kitchen \"window\"\n";
        device.kind = "temperature/humidity";
        device.hasContact = true; device.contactOpen = true;
        device.hasTemperature = true; device.temperatureCelsius = 21.5;
        device.hasHumidity = true; device.humidityPercent = 40.5;
        device.hasOccupancy = true; device.occupied = true;
        device.hasBattery = true; device.batteryPercent = 80;
        device.hasLight = true; device.lightOn = true; device.hasBrightness = true; device.brightnessPercent = 50;
        device.hasRemote = true; device.hasLowTemperatureAlert = true; device.lowTemperatureAlert = 5;
        device.hasHighTemperatureAlert = true; device.highTemperatureAlert = 30;
        device.manufacturerName = "Acme\t"; device.modelId = "Model\\1";
        endpoint.endpoint = 2; endpoint.profileId = 0x0104; endpoint.deviceId = 0x0101;
        endpoint.inputClusters.push_back(0x0006); endpoint.outputClusters.push_back(0x0008);
        device.endpoints.push_back(endpoint);
        gateway->devices_[device.ieeeAddress] = device;
        gateway->addLog("view control\n\001");
    }
    static bool contactAlertIsCancelled(Gateway* gateway, const std::string& ieeeAddress) {
        std::vector<unsigned char> open;
        std::vector<unsigned char> closed;
        open.push_back(1); open.push_back(0);
        closed.push_back(0); closed.push_back(0);
        if (!gateway->applyIasZoneStatus(ieeeAddress, open) ||
            gateway->nextContactAlertAt_.count(ieeeAddress) != 1 ||
            gateway->contactOpenedAt_.count(ieeeAddress) != 1) return false;
        if (!gateway->applyIasZoneStatus(ieeeAddress, closed) ||
            gateway->nextContactAlertAt_.count(ieeeAddress) != 0 ||
            gateway->contactOpenedAt_.count(ieeeAddress) != 0) return false;
        gateway->poll();
        return gateway->logsJson().find("Notification queued: Window open") == std::string::npos &&
               gateway->logsJson().find("Notification suppressed: Window open") == std::string::npos;
    }
    static void logRingAndSerialization() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        for (int i = 0; i < 1400; ++i) {
            std::ostringstream entry;
            entry << "entry-" << i << std::string(760, 'a');
            gateway.addLog(entry.str());
        }
        std::string logs = gateway.logsJson();
        assert(logs.find("entry-0") == std::string::npos);
        assert(logs.find("entry-1399") != std::string::npos);
        gateway.addLog(std::string(800, 'x'));
        gateway.addLog("control\n\t\001");
        logs = gateway.logsJson();
        assert(logs.find("\\n\\t\\u0001") != std::string::npos);
        assert(logs.find(std::string(769, 'x')) == std::string::npos);
        assert(gateway.deviceLogLabel(0x1234) == "unknown node=0x1234");
    }
    static std::vector<unsigned char> decodedLastEzsp(const Gateway& gateway) {
        std::vector<unsigned char> command = gateway.ashTransmitQueue_.empty() ? gateway.ashPendingData_ : gateway.ashTransmitQueue_.back();
        unsigned char random = 0x42;
        for (unsigned long i = 0; i < command.size(); ++i) {
            command[i] ^= random;
            random = (random & 1) ? static_cast<unsigned char>((random >> 1) ^ 0xb8) :
                                   static_cast<unsigned char>(random >> 1);
        }
        return command;
    }
    static void closingPermitReplyTimeout() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        std::string error;
        gateway.online_ = true;
        gateway.networkFormed_ = true;
        gateway.ezspVersion_ = 8;
        gateway.joinUntil_ = time(0) + 120;
        gateway.joinPermitRenewAt_ = time(0) + 100;
        assert(gateway.setJoining(0, &error));
        assert(gateway.joinPermitClosePending_);
        assert(gateway.joinPermitCloseDeadline_ >= time(0) + 9);
        gateway.joinPermitCloseDeadline_ = time(0) - 1;
        assert(gateway.nextPollDeadline() == time(0));
        gateway.poll();
        assert(!gateway.joinPermitClosePending_ && gateway.joinPermitCloseDeadline_ == 0);
        assert(gateway.joinUntil_ == 0 && gateway.joinPermitRenewAt_ == 0);
        assert(gateway.statusJson().find("\"joining_until\":0") != std::string::npos);
        std::vector<unsigned char> retry = decodedLastEzsp(gateway);
        assert(retry[3] == 0x22 && retry[5] == 0);
    }
    static void refreshDiscoversMissingEndpoints() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        Device device;
        std::string error;
        device.ieeeAddress = "0x00124b00000000dd";
        device.networkAddress = 0x4321;
        gateway.devices_[device.ieeeAddress] = device;
        gateway.nodeAddresses_[device.networkAddress] = device.ieeeAddress;
        gateway.online_ = true;
        gateway.networkFormed_ = true;
        assert(gateway.refreshDevice(device.ieeeAddress, &error));
        assert(gateway.interviews_[device.networkAddress].refreshOnly);
        assert(gateway.interviews_[device.networkAddress].operations[0].type == Gateway::InterviewActiveEndpoints);
    }
    static void lightCommandEncoding() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        Device device;
        EndpointDescriptor power;
        EndpointDescriptor level;
        std::string error;
        device.ieeeAddress = "0x00124b00000000cc";
        device.networkAddress = 0x4321;
        device.hasLight = true;
        device.lightOn = false;
        power.endpoint = 3; power.inputClusters.push_back(0x0006);
        level.endpoint = 7; level.inputClusters.push_back(0x0008);
        device.endpoints.push_back(power);
        device.endpoints.push_back(level);
        gateway.devices_[device.ieeeAddress] = device;
        gateway.online_ = true; gateway.networkFormed_ = true; gateway.coordinatorInitializationStep_ = 0; gateway.ezspVersion_ = 8;

        assert(gateway.setLightState(device.ieeeAddress, "on", -1, &error));
        std::vector<unsigned char> command = decodedLastEzsp(gateway);
        assert(command.size() == 24 && command[3] == 0x34 && command[4] == 0x00);
        assert(command[6] == 0x21 && command[7] == 0x43 && command[8] == 0x04 && command[9] == 0x01);
        assert(command[10] == 0x06 && command[11] == 0x00 && command[12] == 1 && command[13] == 3);
        assert(command[20] == 3 && command[21] == 0x11 && command[22] == 1 && command[23] == 1);

        assert(gateway.setLightState(device.ieeeAddress, "off", -1, &error));
        command = decodedLastEzsp(gateway);
        assert(command[12] == 1 && command[13] == 3 && command[20] == 3 && command[23] == 0);
        assert(gateway.setLightState(device.ieeeAddress, "toggle", -1, &error));
        command = decodedLastEzsp(gateway);
        assert(command[23] == 2);

        assert(gateway.setLightState(device.ieeeAddress, "ignored", 50, &error));
        command = decodedLastEzsp(gateway);
        assert(command.size() == 27 && command[10] == 0x08 && command[11] == 0 && command[12] == 1 && command[13] == 7);
        assert(command[20] == 6 && command[21] == 0x11 && command[23] == 0x04 &&
               command[24] == 127 && command[25] == 0 && command[26] == 0);

        assert(!gateway.setLightState("0xmissing", "on", -1, &error));
        assert(error == "device is not a light");
        Device nondimmable = device;
        nondimmable.ieeeAddress = "0x00124b00000000cd";
        nondimmable.endpoints.erase(nondimmable.endpoints.begin() + 1);
        gateway.devices_[nondimmable.ieeeAddress] = nondimmable;
        assert(!gateway.setLightState(nondimmable.ieeeAddress, "on", 1, &error));
        assert(error == "brightness is unavailable for this light");
        assert(!gateway.setLightState(nondimmable.ieeeAddress, "invalid", -1, &error));
        assert(error == "invalid light state");
        Device unsupported = device;
        unsupported.ieeeAddress = "0x00124b00000000ce";
        unsupported.endpoints.clear();
        gateway.devices_[unsupported.ieeeAddress] = unsupported;
        assert(!gateway.setLightState(unsupported.ieeeAddress, "on", -1, &error));
        assert(error == "power control is unavailable for this light");
    }
    static void identifyCommandEncoding() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        Device device;
        EndpointDescriptor clientOnly;
        EndpointDescriptor identify;
        std::string error;
        device.ieeeAddress = "0x00124b00000000ee";
        device.networkAddress = 0x4321;
        clientOnly.endpoint = 1;
        clientOnly.outputClusters.push_back(0x0003);
        identify.endpoint = 4;
        identify.profileId = 0x0104;
        identify.inputClusters.push_back(0x0003);
        device.endpoints.push_back(clientOnly);
        device.endpoints.push_back(identify);
        gateway.devices_[device.ieeeAddress] = device;
        assert(!gateway.identifyDevice(device.ieeeAddress, &error));
        assert(error == "Zigbee network is not ready");
        gateway.online_ = true; gateway.networkFormed_ = true; gateway.ezspVersion_ = 8;
        assert(gateway.identifyDevice(device.ieeeAddress, &error));
        std::vector<unsigned char> command = decodedLastEzsp(gateway);
        assert(command.size() == 26 && command[3] == 0x34 && command[4] == 0x00);
        assert(command[6] == 0x21 && command[7] == 0x43);
        assert(command[8] == 0x04 && command[9] == 0x01); /* endpoint profile */
        assert(command[10] == 0x03 && command[11] == 0x00);
        assert(command[12] == 1 && command[13] == 4);
        assert(command[20] == 5 && command[21] == 0x11 && command[23] == 0x00);
        assert(command[24] == 5 && command[25] == 0);
        assert(!gateway.identifyDevice("missing", &error) && error == "device is unknown");
        gateway.devices_[device.ieeeAddress].networkAddress = 0xffff;
        assert(!gateway.identifyDevice(device.ieeeAddress, &error) && error == "device has no active network address");
        gateway.devices_[device.ieeeAddress].networkAddress = 0x4321;
        gateway.devices_[device.ieeeAddress].endpoints.erase(gateway.devices_[device.ieeeAddress].endpoints.begin() + 1);
        assert(!gateway.identifyDevice(device.ieeeAddress, &error) && error == "device has no Identify server endpoint");
    }
    static void ezspUpgradeCompatibility() {
        for (int protocol = 8; protocol <= 13; protocol += 5) {
            Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
            unsigned char legacy[] = {0, 0x80, 0, static_cast<unsigned char>(protocol), 2, 0x80, 0x67};
            gateway.handleEzspFrame(std::vector<unsigned char>(legacy, legacy + sizeof(legacy)));
            assert(gateway.ezspVersion_ == protocol && !gateway.online_);
            std::vector<unsigned char> reply(9, 0);
            reply[1] = 0x80; reply[2] = 1; reply[5] = protocol; reply[6] = 2;
            reply[7] = 0x80; reply[8] = 0x67;
            gateway.handleEzspFrame(reply);
            assert(gateway.online_ && gateway.coordinatorInitializationStep_ == 1);
            /* Required initialization must fail closed on missing status. */
            reply.resize(5); reply[3] = 0x53;
            gateway.handleEzspFrame(reply);
            assert(!gateway.online_ && gateway.coordinatorInitializationStep_ == 1);
            gateway.online_ = true;
            gateway.coordinatorInitializationStep_ = 11;
            reply.resize(6); reply[5] = 0x37;
            gateway.handleEzspFrame(reply);
            assert(gateway.online_ == (protocol == 13));
            assert(gateway.coordinatorInitializationStep_ == (protocol == 13 ? 12 : 11));
            gateway.coordinatorInitializationStep_ = 0;
            gateway.online_ = true; gateway.networkFormed_ = true;
            gateway.ashTransmitQueue_.clear(); gateway.ashTransmitSensitiveQueue_.clear(); gateway.ashPendingData_.clear();
            gateway.joinPreparationStep_ = 2; gateway.pendingJoinSeconds_ = 60;
            reply.resize(6); reply[3] = 0x55; reply[5] = 0;
            gateway.handleEzspFrame(reply);
            assert(gateway.joinPreparationStep_ == 3);
            assert(gateway.ashTransmitQueue_.size() == 1);
            std::vector<unsigned char> request = gateway.ashTransmitQueue_.back();
            unsigned char mask = 0x42;
            for (unsigned long i = 0; i < request.size(); ++i) {
                request[i] ^= mask;
                mask = (mask & 1) ? static_cast<unsigned char>((mask >> 1) ^ 0xb8) :
                                   static_cast<unsigned char>(mask >> 1);
            }
            assert(request[3] == (protocol == 13 ? 0x11 : 0xaf));
            assert(request[4] == (protocol == 13 ? 1 : 0));
            assert(request.size() == static_cast<unsigned long>(protocol == 13 ? 30 : 29));
            if (protocol == 13) assert(request[29] == 0);
            reply[3] = request[3]; reply[4] = request[4];
            /* A v13 error with a zero low byte is still an error. */
            if (protocol == 13) { reply.resize(9, 0); reply[6] = 1; }
            else reply[5] = 1;
            gateway.handleEzspFrame(reply);
            assert(gateway.joinPreparationStep_ == 0 && gateway.joinUntil_ == 0);
            gateway.joinPreparationStep_ = 3; gateway.pendingJoinSeconds_ = 60;
            for (unsigned long i = 5; i < reply.size(); ++i) reply[i] = 0;
            gateway.handleEzspFrame(reply);
            assert(gateway.joinPreparationStep_ == 4);
            if (protocol == 13) {
                gateway.joinPreparationStep_ = 3;
                reply.resize(6);
                gateway.handleEzspFrame(reply);
                assert(gateway.joinPreparationStep_ == 0);
            }
            gateway.joinPreparationStep_ = 3;
            gateway.joinPreparationDeadline_ = time(0) - 1;
            gateway.nextHeartbeatAt_ = time(0) + 100;
            gateway.poll();
            assert(gateway.joinPreparationStep_ == 0 && gateway.pendingJoinSeconds_ == 0);
        }
        Gateway unsupported("/dev/null", 115200, "/tmp", "", "", "");
        unsigned char reply[] = {0, 0x80, 0, 14, 2, 0, 0};
        unsupported.handleEzspFrame(std::vector<unsigned char>(reply, reply + sizeof(reply)));
        assert(!unsupported.online_ && unsupported.ashTransmitQueue_.empty());
        reply[3] = 13;
        unsupported.handleEzspFrame(std::vector<unsigned char>(reply, reply + 4));
        assert(!unsupported.online_ && unsupported.ezspVersion_ == 4);
    }
    static void networkJoinWindowDoesNotClearNetworkState() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        std::vector<unsigned char> status(6, 0);
        gateway.ezspVersion_ = 13;
        gateway.networkFormed_ = true;
        status[3] = 0x19;
        status[5] = 0x9c; /* NETWORK_OPENED */
        gateway.handleEzspFrame(status);
        assert(gateway.networkFormed_);
        status[5] = 0x9d; /* NETWORK_CLOSED */
        gateway.handleEzspFrame(status);
        assert(gateway.networkFormed_);
        status[5] = 0x91; /* NETWORK_DOWN */
        gateway.handleEzspFrame(status);
        assert(!gateway.networkFormed_);
    }
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
        std::vector<unsigned char> response;
        assert(gateway.interviews_[node].operations[0].type == Gateway::InterviewActiveEndpoints);

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
            if (i >= 3) { response[1] = 0x04; response[2] = 0x01; }
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
        gateway.ashTransmitSensitiveQueue_.clear();
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
    static bool interviewRetryContributesDeadline(Gateway* gateway, unsigned short node) {
        Gateway::InterviewState state;
        Gateway::InterviewOperation operation;
        operation.retryAt = time(0) + 5;
        state.operations.push_back(operation);
        gateway->online_ = true;
        gateway->networkFormed_ = true;
        gateway->coordinatorInitializationStep_ = 0;
        gateway->interviews_[node] = state;
        return gateway->nextPollDeadline() == operation.retryAt;
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
        gateway.logs_.clear(); gateway.logBytes_ = 0;
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
    static bool remoteKeyEventsAreLogged() {
        const unsigned short node = 0x4321;
        const std::string ieee = "0x00124b0000000021";
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        EndpointDescriptor endpoint;
        std::vector<unsigned char> move(1, 80);
        endpoint.endpoint = 1;
        endpoint.outputClusters.push_back(0x0006);
        endpoint.outputClusters.push_back(0x0008);
        gateway.nodeAddresses_[node] = ieee;
        gateway.devices_[ieee].ieeeAddress = ieee;
        gateway.devices_[ieee].endpoints.push_back(endpoint);
        gateway.classifyDevice(&gateway.devices_[ieee]);
        gateway.logs_.clear(); gateway.logBytes_ = 0;
        gateway.logRemoteKeyEvent(node, 0x0006, 1, 0, 0x01, std::vector<unsigned char>());
        gateway.logRemoteKeyEvent(node, 0x0008, 1, 0, 0x05, move);
        return gateway.logsJson().find("Remote key event") != std::string::npos &&
               gateway.logsJson().find("action=on") != std::string::npos &&
               gateway.logsJson().find("action=move") != std::string::npos;
    }
    static void tradfriFirmwareBindingAndOta() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        const std::string ieee = "0x70ac08fffe75827b";
        gateway.nodeAddresses_[0xd938] = ieee;
        Device& device = gateway.devices_[ieee];
        device.ieeeAddress = ieee;
        device.modelId = "TRADFRI on/off switch";
        gateway.coordinatorIeeeAddress_.assign(8, 0x11);
        const char* versions[] = {"2.2.010", "2.3.074", "2.3.075", "24.4.6"};
        for (unsigned int i = 0; i < 4; ++i) {
            device.softwareBuildId = versions[i];
            gateway.logs_.clear(); gateway.logBytes_ = 0;
            gateway.sendZdoBindRequest(0xd938, 1, 6, ieee, 42, 1);
            const std::string expected = i < 2 ? "payload=2a7b8275feff08ac70010600018503" :
                "payload=2a7b8275feff08ac7001060003111111111111111101";
            assert(gateway.logsJson().find(expected) != std::string::npos);
        }
        std::vector<unsigned char> incoming(19, 0);
        incoming[1] = 4; incoming[2] = 1; incoming[3] = 0x19;
        incoming[5] = 1; incoming[6] = 1;
        incoming[14] = 0x38; incoming[15] = 0xd9;
        const unsigned char query[] = {1, 14, 1, 1, 0x7c, 0x11, 0xc5, 0x11, 0x31, 6, 1, 0x22, 1, 0};
        incoming[18] = sizeof(query);
        incoming.insert(incoming.end(), query, query + sizeof(query));
        gateway.logs_.clear(); gateway.logBytes_ = 0;
        gateway.handleIncomingMessage(incoming);
        assert(gateway.logsJson().find("payload=190e0298") != std::string::npos);
        EndpointDescriptor endpoint;
        endpoint.endpoint = 1;
        endpoint.outputClusters.push_back(6);
        device.endpoints.push_back(endpoint);
        device.hasRemote = true;
        incoming.resize(22);
        incoming[3] = 6; incoming[18] = 3;
        incoming[19] = 1; incoming[20] = 42; incoming[21] = 1;
        for (unsigned char type = 0; type <= 5; ++type) {
            incoming[0] = type;
            incoming[6] = type < 2 ? 1 : 255;
            gateway.logs_.clear();
            gateway.handleIncomingMessage(incoming);
            assert(gateway.logsJson().find("action=on") != std::string::npos);
            assert((gateway.logsJson().find("payload=182a0b0100") != std::string::npos) == (type < 2));
        }
    }
    static bool nativeRemoteBindingFlow() {
        const std::string remoteIeee = "0x00124b0000000099";
        const std::string lightIeee = "0x00124b0000000042";
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        EndpointDescriptor remoteEndpoint, lightEndpoint;
        remoteEndpoint.endpoint = 1;
        remoteEndpoint.outputClusters.push_back(0x0006);
        lightEndpoint.endpoint = 1;
        lightEndpoint.inputClusters.push_back(0x0004);
        lightEndpoint.inputClusters.push_back(0x0006);
        Device& remote = gateway.devices_[remoteIeee];
        remote.ieeeAddress = remoteIeee; remote.networkAddress = 0x1234;
        remote.hasRemote = true; remote.endpoints.push_back(remoteEndpoint);
        Device& light = gateway.devices_[lightIeee];
        light.ieeeAddress = lightIeee; light.networkAddress = 0x2345;
        light.hasLight = true; light.endpoints.push_back(lightEndpoint);
        gateway.nodeAddresses_[0x1234] = remoteIeee;
        gateway.coordinatorIeeeAddress_.assign(8, 0x11);
        gateway.online_ = true; gateway.networkFormed_ = true;
        gateway.coordinatorInitializationStep_ = 0;
        SceneGroup group; group.id = 42; group.name = "Room"; group.members.push_back(lightIeee);
        gateway.sceneGroups_.push_back(group);
        NativeRemoteBinding binding; binding.ieeeAddress = remoteIeee; binding.groupId = 42;
        gateway.nativeBindings_.push_back(binding);
        gateway.queueNativeBindingChanges(std::vector<NativeRemoteBinding>(), std::vector<SceneGroup>());
        if (gateway.nativeBindOperations_.size() != 2 ||
            !gateway.nativeBindOperations_[0].unbind ||
            gateway.nativeBindOperations_[1].groupId != 42) return false;
        gateway.pollNativeBindings();
        if (gateway.nativeBindOperations_[0].deadline == 0 ||
            gateway.logsJson().find("cluster=0x22") == std::string::npos) return false;
        std::vector<unsigned char> reply(21, 0);
        reply[3] = 0x22; reply[4] = 0x80;
        reply[14] = 0x34; reply[15] = 0x12; reply[18] = 2;
        reply[19] = gateway.nativeBindOperations_[0].sequence;
        reply[20] = 0x88; /* no old coordinator entry */
        gateway.handleIncomingMessage(reply);
        if (gateway.nativeBindOperations_.size() != 1) return false;
        gateway.pollNativeBindings();
        if (gateway.nativeBindOperations_[0].deadline == 0 ||
            gateway.logsJson().find("payload=") == std::string::npos) return false;
        reply[3] = 0x21; reply[19] = gateway.nativeBindOperations_[0].sequence;
        reply[20] = 0x84; /* group unsupported: bind to lights */
        gateway.handleIncomingMessage(reply);
        if (gateway.nativeBindOperations_.size() != 1 ||
            gateway.nativeBindOperations_[0].destinationMode != 3 ||
            gateway.nativeBindOperations_[0].targetIeee != lightIeee) return false;
        gateway.pollNativeBindings();
        reply[19] = gateway.nativeBindOperations_[0].sequence;
        reply[20] = 0;
        gateway.handleIncomingMessage(reply);
        if (!gateway.nativeBindOperations_.empty() ||
            gateway.nativeBindingStates_[remoteIeee] != "active") return false;
        RemoteSceneBinding sceneRule; sceneRule.ieeeAddress = remoteIeee; sceneRule.action = "on";
        sceneRule.groupId = 42; sceneRule.sceneIds.push_back(1);
        gateway.sceneBindings_.push_back(sceneRule);
        gateway.dispatchRemoteSceneAction(remoteIeee, "on");
        return gateway.sceneRetries_.empty();
    }
    static bool nativeIkeaFirmwareModes() {
        const std::string remoteIeee = "0x70ac08fffe75827b";
        const std::string lightIeee = "0x84ba20fffea0e73c";
        const char* versions[] = {"2.3.074", "2.3.075"};
        for (unsigned int v = 0; v < 2; ++v) {
            Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
            Device& remote = gateway.devices_[remoteIeee];
            EndpointDescriptor endpoint; endpoint.endpoint = 1; endpoint.outputClusters.push_back(6);
            remote.ieeeAddress = remoteIeee; remote.modelId = "TRADFRI on/off switch";
            remote.softwareBuildId = versions[v]; remote.hasRemote = true;
            remote.endpoints.push_back(endpoint);
            Device& light = gateway.devices_[lightIeee];
            endpoint.outputClusters.clear(); endpoint.inputClusters.push_back(4); endpoint.inputClusters.push_back(6);
            light.ieeeAddress = lightIeee; light.hasLight = true; light.endpoints.push_back(endpoint);
            SceneGroup group; group.id = 42; group.members.push_back(lightIeee);
            gateway.sceneGroups_.push_back(group);
            NativeRemoteBinding binding; binding.ieeeAddress = remoteIeee; binding.groupId = 42;
            gateway.nativeBindings_.push_back(binding);
            gateway.queueNativeBindingChanges(std::vector<NativeRemoteBinding>(), std::vector<SceneGroup>());
            if (gateway.nativeBindOperations_.size() != 2) return false;
            if (gateway.nativeBindOperations_[0].destinationMode != (v == 0 ? 1 : 3)) return false;
            if (gateway.nativeBindOperations_[1].destinationMode != (v == 0 ? 1 : 3)) return false;
            if (v == 0 && gateway.nativeBindOperations_[1].groupId != 42) return false;
            if (v == 1 && gateway.nativeBindOperations_[1].targetIeee != lightIeee) return false;
        }
        return true;
    }
    static bool styrbarArrowEventsAreLogged() {
        const unsigned short node = 0x4321;
        const std::string ieee = "0x00124b0000000022";
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        EndpointDescriptor endpoint;
        std::vector<unsigned char> incoming(18, 0);
        endpoint.endpoint = 1;
        endpoint.outputClusters.push_back(0x0003);
        endpoint.outputClusters.push_back(0x0006);
        endpoint.outputClusters.push_back(0x0008);
        endpoint.outputClusters.push_back(0x0019);
        endpoint.outputClusters.push_back(0x1000);
        gateway.nodeAddresses_[node] = ieee;
        gateway.devices_[ieee].ieeeAddress = ieee;
        gateway.devices_[ieee].name = "Styrbar";
        gateway.devices_[ieee].endpoints.push_back(endpoint);
        gateway.classifyDevice(&gateway.devices_[ieee]);
        incoming[1] = 0x04; incoming[2] = 0x01; /* Home Automation */
        incoming[3] = 0x05; incoming[4] = 0x00; /* Scenes */
        incoming[5] = 1; incoming[6] = 1;
        incoming[14] = static_cast<unsigned char>(node); incoming[15] = static_cast<unsigned char>(node >> 8);
        incoming.push_back(9);
        incoming.push_back(0x05); incoming.push_back(0x7c); incoming.push_back(0x11);
        incoming.push_back(42); incoming.push_back(0x07);
        incoming.push_back(0x01); incoming.push_back(0x01); incoming.push_back(13); incoming.push_back(0);
        gateway.handleIncomingMessage(incoming);
        incoming[22] = 43; /* transaction sequence */
        incoming[24] = 0x00; /* group 0x0100, right */
        gateway.handleIncomingMessage(incoming);
        incoming[18] = 8;
        incoming[22] = 44;
        incoming[23] = 0x08;
        incoming[24] = 0x01; incoming[25] = 13; incoming[26] = 0;
        gateway.handleIncomingMessage(incoming);
        incoming[18] = 7;
        incoming[22] = 45;
        incoming[23] = 0x09;
        incoming[24] = 0; incoming[25] = 0;
        gateway.handleIncomingMessage(incoming);
        return gateway.logsJson().find("action=arrow_left_click") != std::string::npos &&
               gateway.logsJson().find("action=arrow_right_click") != std::string::npos &&
               gateway.logsJson().find("action=arrow_left_hold") != std::string::npos &&
               gateway.logsJson().find("action=arrow_left_release") != std::string::npos &&
               gateway.logsJson().find("payload=182a0b0700") != std::string::npos &&
               gateway.logsJson().find("Configure reporting response from 0x00124b0000000022: cluster 0x5") == std::string::npos;
    }
    static bool remoteBindingsAndIgnoredEvents() {
        const unsigned short node = 0x1234;
        const std::string remoteIeee = "remote";
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        Device& remote = gateway.devices_[remoteIeee];
        EndpointDescriptor remoteEndpoint; remoteEndpoint.endpoint = 1; remoteEndpoint.outputClusters.push_back(0x0006);
        remote.ieeeAddress = remoteIeee; remote.hasRemote = true; remote.endpoints.push_back(remoteEndpoint);
        gateway.nodeAddresses_[node] = remoteIeee;
        Device& light = gateway.devices_["light"];
        EndpointDescriptor lightEndpoint; lightEndpoint.endpoint = 1; lightEndpoint.inputClusters.push_back(0x0004);
        lightEndpoint.inputClusters.push_back(0x0005); lightEndpoint.inputClusters.push_back(0x0006);
        light.ieeeAddress = "light"; light.networkAddress = 0x2222; light.hasLight = true; light.endpoints.push_back(lightEndpoint);
        SceneGroup group; group.id = 10; group.name = "Room"; group.members.push_back("light");
        Scene scene; scene.id = 1; scene.name = "Bright";
        SceneLightState state; state.ieeeAddress = "light"; state.on = true; state.brightnessPercent = -1;
        scene.states.push_back(state); group.scenes.push_back(scene); gateway.sceneGroups_.push_back(group);
        RemoteSceneBinding binding; binding.ieeeAddress = remoteIeee; binding.action = "arrow_left";
        binding.groupId = 10; binding.sceneIds.push_back(1); gateway.sceneBindings_.push_back(binding);
        gateway.online_ = true; gateway.networkFormed_ = true; gateway.coordinatorInitializationStep_ = 0;
        std::vector<unsigned char> leftClick; leftClick.push_back(0x01); leftClick.push_back(0x01); leftClick.push_back(13); leftClick.push_back(0);
        if (!gateway.logRemoteKeyEvent(node, 0x0005, 1, 0x117c, 0x07, leftClick) ||
            gateway.sceneRetries_.find(10) == gateway.sceneRetries_.end() || gateway.sceneRetries_[10].sceneId != 1) return false;
        if (!gateway.logRemoteKeyEvent(node, 0x0005, 1, 0x117c, 0x07, leftClick) ||
            !gateway.sceneRetries_[10].retryRequested || gateway.selectedScenes_.count(10) != 0) return false;
        std::vector<unsigned char> unknownGroup; unknownGroup.push_back(0x02); unknownGroup.push_back(0); unknownGroup.push_back(13); unknownGroup.push_back(0);
        if (gateway.logRemoteKeyEvent(node, 0x0005, 1, 0x117c, 0x07, unknownGroup)) return false;
        std::vector<unsigned char> hold; hold.push_back(1); hold.push_back(0); hold.push_back(0);
        if (!gateway.logRemoteKeyEvent(node, 0x0005, 1, 0x117c, 0x08, hold) ||
            !gateway.logRemoteKeyEvent(node, 0x0005, 1, 0x117c, 0x09, std::vector<unsigned char>()) ||
            gateway.logRemoteKeyEvent(node, 0x0005, 1, 0x117c, 0x09, std::vector<unsigned char>())) return false;
        if (gateway.logRemoteKeyEvent(0x9999, 0x0006, 1, 0, 0x01, std::vector<unsigned char>())) return false;
        return gateway.sceneRetries_[10].sceneId == 1 && gateway.sceneRetries_[10].retryRequested &&
               gateway.selectedScenes_.count(10) == 0;
    }
    static bool rejectsDuplicateSceneStates() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        std::string error;
        gateway.devices_["0xlight1"].ieeeAddress = "0xlight1";
        gateway.devices_["0xlight1"].hasLight = true;
        gateway.devices_["0xlight2"].ieeeAddress = "0xlight2";
        gateway.devices_["0xlight2"].hasLight = true;
        const char* configuration =
            "G\t1\tTest\nM\t1\t0xlight1\nM\t1\t0xlight2\nS\t1\t1\tScene\n"
            "T\t1\t1\t0xlight1\t1\t50\nT\t1\t1\t0xlight1\t0\t-1\n";
        return !gateway.setSceneConfiguration(configuration, &error) && error == "invalid scene configuration";
    }
    static bool rejectsInvalidSceneCrossReferences() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        Device light; light.ieeeAddress = "light1"; light.hasLight = true;
        Device otherLight; otherLight.ieeeAddress = "light2"; otherLight.hasLight = true;
        Device remote; remote.ieeeAddress = "remote1"; remote.hasRemote = true;
        gateway.devices_[light.ieeeAddress] = light;
        gateway.devices_[otherLight.ieeeAddress] = otherLight;
        gateway.devices_[remote.ieeeAddress] = remote;
        SceneGroup oldGroup; oldGroup.id = 77; oldGroup.name = "old";
        gateway.sceneGroups_.push_back(oldGroup);
        const char* invalid[] = {
            "G\t1\tRoom\nM\t1\tlight1\nS\t1\t1\tScene\nT\t1\t1\tlight1\t1\t50\nB\tremote1\ton\t2\t0\t1\n",
            "G\t1\tRoom\nM\t1\tlight1\nS\t1\t1\tScene\nT\t1\t1\tlight1\t1\t50\nB\tremote1\ton\t1\t0\t2\n",
            "G\t1\tRoom\nM\t1\tlight1\nS\t1\t1\tScene\nT\t1\t1\tlight1\t1\t50\nB\tmissing\ton\t1\t0\t1\n",
            "G\t1\tRoom\nM\t1\tlight1\nS\t1\t1\tScene\n",
            "G\t1\tRoom\nM\t1\tlight1\nS\t1\t1\tScene\nT\t1\t1\tlight2\t1\t50\n",
            "G\t1\tRoom\tTabbed\n",
            "G\t1\tRoom\nM\t1\tlight1\nS\t1\t1\tScene\tTabbed\nT\t1\t1\tlight1\t1\t50\n"
        };
        for (unsigned long i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            std::string error;
            if (gateway.setSceneConfiguration(invalid[i], &error) || error.empty() ||
                gateway.sceneGroups_.size() != 1 || gateway.sceneGroups_[0].id != 77 ||
                gateway.sceneGroups_[0].name != "old") return false;
        }
        return true;
    }
    static bool saveTriggeredProvisioning() {
        std::ostringstream path;
        path << "/tmp/microhad-zigbee-save-provision-" << static_cast<long>(getpid());
        std::string error;
        if (!ensureDirectory(path.str(), &error)) return false;
        const std::string lightIeee = "0x00124b0000000042";
        const std::string remoteIeee = "0x00124b0000000099";
        const std::string encoded =
            "G\t42\tKitchen\nM\t42\t0x00124b0000000042\nS\t42\t7\tDinner\n"
            "T\t42\t7\t0x00124b0000000042\t1\t50\nN\t0x00124b0000000099\t42\n";
        {
            Gateway gateway("/dev/null", 115200, path.str(), "", "", "");
            Device& light = gateway.devices_[lightIeee];
            EndpointDescriptor lightEndpoint;
            light.ieeeAddress = lightIeee; light.hasLight = true; light.networkAddress = 0x2345;
            lightEndpoint.endpoint = 1;
            lightEndpoint.inputClusters.push_back(4); lightEndpoint.inputClusters.push_back(5);
            lightEndpoint.inputClusters.push_back(6); lightEndpoint.inputClusters.push_back(8);
            light.endpoints.push_back(lightEndpoint);
            Device& remote = gateway.devices_[remoteIeee];
            EndpointDescriptor remoteEndpoint;
            remote.ieeeAddress = remoteIeee; remote.hasRemote = true; remote.networkAddress = 0x1234;
            remoteEndpoint.endpoint = 1; remoteEndpoint.outputClusters.push_back(6);
            remote.endpoints.push_back(remoteEndpoint);
            gateway.online_ = true; gateway.networkFormed_ = true;
            if (!gateway.setSceneConfiguration(encoded, &error)) return false;
            if (gateway.sceneProvisioning_[lightIeee].commands.size() != 2 ||
                gateway.sceneProvisioning_[lightIeee].deadline == 0 ||
                gateway.nativeBindOperations_.empty()) return false;
            const unsigned char transaction = gateway.sceneProvisioning_[lightIeee].commands[0].transaction;
            std::vector<unsigned char> response;
            response.push_back(0x09); response.push_back(transaction); response.push_back(0);
            response.push_back(0); response.push_back(42); response.push_back(0);
            if (gateway.handleSceneProvisionResponse(lightIeee, 4, transaction + 1, 0, response, 3) ||
                gateway.sceneProvisioning_[lightIeee].index != 0) return false;
            if (!gateway.handleSceneProvisionResponse(lightIeee, 4, transaction, 0, response, 3) ||
                gateway.sceneProvisioning_[lightIeee].index != 1) return false;
            gateway.pollSceneProvisioning();
            Gateway::SceneProvisionState& pending = gateway.sceneProvisioning_[lightIeee];
            if (pending.deadline == 0 || pending.commands[1].transaction == 0) return false;
            pending.deadline = time(0) - 1;
            gateway.pollSceneProvisioning();
            if (!pending.waitingForTraffic || pending.deadline != 0) return false;
            gateway.wakeSceneProvisioning(lightIeee);
            gateway.pollSceneProvisioning();
            if (pending.waitingForTraffic || pending.deadline == 0 || pending.index != 1) return false;
            response[1] = pending.commands[1].transaction;
            response.push_back(7);
            if (!gateway.handleSceneProvisionResponse(lightIeee, 5, response[1], 0, response, 3) ||
                !gateway.sceneProvisioning_.empty()) return false;
            if (!gateway.setSceneConfiguration(encoded, &error) || gateway.nativeBindOperations_.empty() ||
                gateway.sceneProvisioning_.empty()) return false;
        }
        {
            Gateway restarted("/dev/null", 115200, path.str(), "", "", "");
            Device& light = restarted.devices_[lightIeee];
            EndpointDescriptor endpoint;
            light.ieeeAddress = lightIeee; light.networkAddress = 0x2345; light.hasLight = true;
            endpoint.endpoint = 1;
            endpoint.inputClusters.push_back(4); endpoint.inputClusters.push_back(5);
            endpoint.inputClusters.push_back(6); endpoint.inputClusters.push_back(8);
            light.endpoints.push_back(endpoint);
            restarted.loadScenes();
            if (!restarted.sceneProvisioning_.empty() || !restarted.nativeBindOperations_.empty() ||
                restarted.nativeBindingStates_[remoteIeee] != "configured; not verified since restart") return false;
            restarted.ezspVersion_ = 13;
            std::vector<unsigned char> initialized(6, 0);
            initialized[3] = 0x17;
            restarted.networkFormed_ = true;
            restarted.handleEzspFrame(initialized);
            initialized[3] = 0x19; initialized[5] = 0x90;
            restarted.handleEzspFrame(initialized);
            if (!restarted.sceneProvisioning_.empty() || !restarted.nativeBindOperations_.empty() ||
                restarted.logsJson().find("FRAME TX recipient=") != std::string::npos) return false;
        }
        unlink((path.str() + "/scenes.conf").c_str());
        return rmdir(path.str().c_str()) == 0;
    }
    static bool sceneBusyDoesNotAdvanceSelection() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        Gateway::SceneRetry& retry = gateway.sceneRetries_[1];
        std::vector<unsigned char> reply(6, 0);
        gateway.ezspVersion_ = 8;
        retry.inFlight = true; retry.ezspSequence = 42; retry.sceneId = 2; retry.sentSceneId = 2;
        reply[0] = 42; reply[3] = 0x38; reply[4] = 0; reply[5] = 0xa1; /* EMBER_NETWORK_BUSY */
        gateway.handleEzspFrame(reply);
        long now = time(0);
        if (retry.inFlight || retry.attempts != 1 || retry.retryAt < now || retry.retryAt > now + 1 || !gateway.selectedScenes_.empty()) return false;
        for (int i = 2; i <= 5; ++i) {
            now = time(0);
            if (!gateway.scheduleSceneRetry(&retry, now) || retry.attempts != i || retry.retryAt != now + 1) return false;
        }
        now = time(0);
        if (!gateway.scheduleSceneRetry(&retry, now) || retry.attempts != 6 || retry.retryAt != now + 10) return false;
        return !gateway.scheduleSceneRetry(&retry, time(0)) && retry.retryAt == LONG_MAX;
    }
    static bool sceneRuntimePersistence() {
        const char* filename = "/tmp/microhad-zigbee-scenes-runtime";
        std::ifstream previousInput(filename);
        bool hadPrevious = previousInput.good();
        std::string previous((std::istreambuf_iterator<char>(previousInput)), std::istreambuf_iterator<char>());
        bool result = false;
        {
            std::ofstream output(filename, std::ios::trunc);
            if (output) { output << "S 12 4\nF 12 1\n"; output.close(); }
            if (output) {
                Gateway loaded("/dev/null", 115200, "/tmp", "", "", "");
                loaded.loadSceneRuntime();
                bool loadedOk = loaded.selectedScenes_.count(12) == 1 && loaded.selectedScenes_[12] == 4 &&
                                loaded.sceneGroupOff_.count(12) == 1 && loaded.sceneGroupOff_[12];
                loaded.selectedScenes_[13] = 9;
                loaded.sceneGroupOff_[13] = false;
                loaded.saveSceneRuntime();
                Gateway restarted("/dev/null", 115200, "/tmp", "", "", "");
                restarted.loadSceneRuntime();
                result = loadedOk && restarted.selectedScenes_.count(13) == 1 && restarted.selectedScenes_[13] == 9 &&
                         restarted.sceneGroupOff_.count(13) == 1 && !restarted.sceneGroupOff_[13];
            }
        }
        if (hadPrevious) {
            std::ofstream restore(filename, std::ios::trunc);
            if (restore) { restore << previous; restore.close(); }
            if (!restore) result = false;
        } else if (unlink(filename) != 0) result = false;
        return result;
    }
    static bool nativeAndFallbackSceneOutcomes() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        gateway.online_ = true; gateway.networkFormed_ = true;
        gateway.coordinatorInitializationStep_ = 0; gateway.ezspVersion_ = 8;
        EndpointDescriptor nativeEndpoint; nativeEndpoint.endpoint = 1;
        nativeEndpoint.inputClusters.push_back(0x0004); nativeEndpoint.inputClusters.push_back(0x0005);
        nativeEndpoint.inputClusters.push_back(0x0006);
        EndpointDescriptor fallbackEndpoint; fallbackEndpoint.endpoint = 1; fallbackEndpoint.inputClusters.push_back(0x0006);
        Device& native = gateway.devices_["native"];
        native.ieeeAddress = "native"; native.networkAddress = 0x1001; native.hasLight = true;
        native.endpoints.push_back(nativeEndpoint);
        Device& fallback = gateway.devices_["fallback"];
        fallback.ieeeAddress = "fallback"; fallback.networkAddress = 0x1002; fallback.hasLight = true;
        fallback.endpoints.push_back(fallbackEndpoint);
        SceneGroup mixed; mixed.id = 3; mixed.name = "Mixed";
        mixed.members.push_back("native"); mixed.members.push_back("fallback");
        Scene scene; scene.id = 1; scene.name = "On";
        SceneLightState nativeState; nativeState.ieeeAddress = "native"; nativeState.on = true; nativeState.brightnessPercent = -1;
        SceneLightState fallbackState; fallbackState.ieeeAddress = "fallback"; fallbackState.on = true; fallbackState.brightnessPercent = -1;
        scene.states.push_back(nativeState); scene.states.push_back(fallbackState); mixed.scenes.push_back(scene);
        gateway.sceneGroups_.push_back(mixed);
        std::string error;
        if (!gateway.activateScene(3, 1, &error) || !gateway.sceneRetries_[3].inFlight ||
            gateway.logsJson().find("profile=0x104 cluster=0x5") == std::string::npos ||
            gateway.devices_["fallback"].lightOn || gateway.selectedScenes_.count(3) != 0) return false;
        std::vector<unsigned char> reply(6, 0);
        reply[0] = gateway.sceneRetries_[3].ezspSequence; reply[3] = 0x38; reply[5] = 0;
        gateway.handleEzspFrame(reply);
        if (gateway.selectedScenes_[3] != 1 || !gateway.devices_["fallback"].lightOn ||
            gateway.devices_["native"].lightOn || gateway.logsJson().find("Scene recalled: Mixed / On") == std::string::npos) return false;
        SceneGroup fallbackOnly; fallbackOnly.id = 4; fallbackOnly.name = "Fallback";
        fallbackOnly.members.push_back("fallback");
        Scene fallbackScene; fallbackScene.id = 2; fallbackScene.name = "Off";
        SceneLightState offState; offState.ieeeAddress = "fallback"; offState.on = false; offState.brightnessPercent = -1;
        fallbackScene.states.push_back(offState); fallbackOnly.scenes.push_back(fallbackScene);
        gateway.sceneGroups_.push_back(fallbackOnly);
        if (!gateway.activateScene(4, 2, &error) || gateway.sceneRetries_[4].retryAt != LONG_MAX ||
            gateway.selectedScenes_[4] != 2 || gateway.devices_["fallback"].lightOn ||
            gateway.logsJson().find("profile=0x104 cluster=0x6") == std::string::npos) return false;
        return true;
    }
    static void sceneBusyUnicastFallback() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        gateway.ezspVersion_ = 8;
        gateway.online_ = true; gateway.networkFormed_ = true;
        gateway.coordinatorInitializationStep_ = 0;
        SceneGroup group; group.id = 1; group.name = "Test";
        Scene scene; scene.id = 2; scene.name = "Scene";
        for (int i = 0; i < 2; ++i) {
            std::string ieee = i == 0 ? "0xlight1" : "0xlight2";
            Device& device = gateway.devices_[ieee];
            device.ieeeAddress = ieee; device.networkAddress = 0x1234 + i;
            EndpointDescriptor endpoint; endpoint.endpoint = 3; endpoint.profileId = 0x0104;
            endpoint.inputClusters.push_back(4); endpoint.inputClusters.push_back(5);
            endpoint.inputClusters.push_back(6);
            device.endpoints.push_back(endpoint);
            group.members.push_back(ieee);
            SceneLightState state; state.ieeeAddress = ieee; state.on = true; state.brightnessPercent = -1;
            scene.states.push_back(state);
        }
        group.scenes.push_back(scene); gateway.sceneGroups_.push_back(group);
        std::string error;
        assert(gateway.activateScene(1, 2, &error));
        Gateway::SceneRetry& retry = gateway.sceneRetries_[1];
        std::vector<unsigned char> reply(6, 0);
        reply[0] = retry.ezspSequence; reply[3] = 0x38; reply[5] = 0xa1;
        gateway.handleEzspFrame(reply);
        assert(retry.unicast && !retry.inFlight && gateway.selectedScenes_.empty());
        gateway.pollSceneRetries();
        assert(retry.inFlight && retry.memberIndex == 0);
        assert(gateway.logsJson().find("dst_ep=3") != std::string::npos);
        reply[0] = retry.ezspSequence; reply[3] = 0x34; reply[5] = 0xa1;
        gateway.handleEzspFrame(reply);
        assert(!retry.inFlight && retry.memberIndex == 0 && gateway.selectedScenes_.empty());
        retry.retryAt = time(0);
        gateway.pollSceneRetries();
        for (int i = 0; i < 2; ++i) {
            reply[0] = retry.ezspSequence; reply[5] = 0;
            gateway.handleEzspFrame(reply);
            assert(gateway.selectedScenes_.empty());
            gateway.pollSceneRetries();
        }
        assert(gateway.selectedScenes_[1] == 2 && retry.retryAt == LONG_MAX);
        assert(gateway.turnOffSceneGroup(1, &error));
        assert(!retry.unicast);
        reply[0] = retry.ezspSequence; reply[3] = 0x38; reply[5] = 0xa1;
        gateway.handleEzspFrame(reply); gateway.pollSceneRetries();
        assert(retry.unicast && retry.inFlight);
        /* A new selection supersedes an outstanding fallback, even on timeout. */
        assert(gateway.activateScene(1, 2, &error));
        retry.sentAt = time(0) - 3;
        gateway.pollSceneRetries();
        assert(!retry.unicast && retry.memberIndex == 0 && retry.inFlight && !retry.sentOff);
    }
    static bool pendingSceneIgnoresRemoteAction() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        RemoteSceneBinding binding;
        Gateway::SceneRetry& retry = gateway.sceneRetries_[1];
        binding.ieeeAddress = "0xremote";
        binding.action = "on";
        binding.groupId = 1;
        binding.sceneIds.push_back(1);
        binding.sceneIds.push_back(2);
        gateway.sceneBindings_.push_back(binding);
        retry.sceneId = 1;
        retry.inFlight = true;
        gateway.dispatchRemoteSceneAction("0xremote", "on");
        if (retry.sceneId != 1 || !retry.retryRequested ||
            gateway.logsJson().find("Scene selection pending; immediate retry requested") == std::string::npos) return false;
        gateway.ezspVersion_ = 8;
        retry.ezspSequence = 42;
        std::vector<unsigned char> reply(6, 0);
        reply[0] = 42; reply[3] = 0x38; reply[4] = 0; reply[5] = 0xa1;
        gateway.handleEzspFrame(reply);
        return !retry.inFlight && !retry.retryRequested && retry.retryAt <= time(0);
    }
};

static void testAshRoundTrip() {
    std::vector<unsigned char> payload;
    std::vector<unsigned char> encoded;
    std::vector<unsigned char> frame;
    AshTransport transport;
    payload.push_back(0x7e);
    payload.push_back(0x7d);
    payload.push_back(0x11);
    payload.push_back(0x13);
    payload.push_back(0x18);
    payload.push_back(0x1a);
    payload.push_back(0x12);
    encoded = AshTransport::encodeData(0x00, payload);
    assert(encoded.size() >= 16);
    assert(encoded[0] == 0 && encoded[1] == 0x7d && encoded[2] == 0x5e &&
           encoded[3] == 0x7d && encoded[4] == 0x5d && encoded[5] == 0x7d && encoded[6] == 0x31 &&
           encoded[7] == 0x7d && encoded[8] == 0x33 && encoded[9] == 0x7d && encoded[10] == 0x38 &&
           encoded[11] == 0x7d && encoded[12] == 0x3a && encoded[13] == 0x12 && encoded[encoded.size() - 1] == 0x7e);
    transport.feed(&encoded[0], encoded.size());
    assert(transport.popFrame(&frame));
    assert(frame.size() == 8);
    assert(frame[0] == 0x00 && frame[1] == 0x7e && frame[2] == 0x7d && frame[3] == 0x11 &&
           frame[4] == 0x13 && frame[5] == 0x18 && frame[6] == 0x1a && frame[7] == 0x12);
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

static void testAshFragmentationAndFrameOrdering() {
    AshTransport transport;
    std::vector<unsigned char> empty;
    std::vector<unsigned char> firstPayload;
    std::vector<unsigned char> secondPayload;
    std::vector<unsigned char> emptyFrame;
    std::vector<unsigned char> firstFrame;
    std::vector<unsigned char> secondFrame;
    std::vector<unsigned char> combined;
    std::vector<unsigned char> decoded;

    firstPayload.push_back(0x41);
    secondPayload.push_back(0x42);
    secondPayload.push_back(0x43);
    emptyFrame = AshTransport::encodeData(0x00, empty);
    firstFrame = AshTransport::encodeData(0x01, firstPayload);
    secondFrame = AshTransport::encodeData(0x02, secondPayload);

    /* A frame split at every meaningful boundary is not visible early. */
    transport.feed(&firstFrame[0], firstFrame.size() - 1);
    assert(!transport.popFrame(&decoded));
    transport.feed(&firstFrame[firstFrame.size() - 1], 1);
    assert(transport.popFrame(&decoded));
    assert(decoded.size() == 2 && decoded[0] == 0x01 && decoded[1] == 0x41);
    assert(!transport.popFrame(&decoded));

    /* Empty and back-to-back frames retain their ordering in one serial read. */
    combined.insert(combined.end(), emptyFrame.begin(), emptyFrame.end());
    combined.insert(combined.end(), firstFrame.begin(), firstFrame.end());
    combined.insert(combined.end(), secondFrame.begin(), secondFrame.end());
    transport.feed(&combined[0], combined.size());
    assert(transport.popFrame(&decoded));
    assert(decoded.size() == 1 && decoded[0] == 0x00);
    assert(transport.popFrame(&decoded));
    assert(decoded.size() == 2 && decoded[0] == 0x01 && decoded[1] == 0x41);
    assert(transport.popFrame(&decoded));
    assert(decoded.size() == 3 && decoded[0] == 0x02 && decoded[1] == 0x42 && decoded[2] == 0x43);
    assert(!transport.popFrame(&decoded));
}

static void testAshRejectsMalformedAndOverlongInput() {
    AshTransport transport;
    std::vector<unsigned char> payload;
    std::vector<unsigned char> encoded;
    std::vector<unsigned char> frame;
    std::vector<unsigned char> overlong(4097, 0x55);
    unsigned char strayDelimiter = 0x7e;
    unsigned char cancel = 0x1a;
    unsigned char malformedEscape[] = { 0x00, 0x7d, 0x00, 0x00, 0x00, 0x7e };

    /* Empty delimiters and invalid escaping are discarded without a frame. */
    transport.feed(&strayDelimiter, 1);
    transport.feed(malformedEscape, sizeof(malformedEscape));
    assert(!transport.popFrame(&frame));

    /* A reset drops a partial frame and oversized unterminated input recovers. */
    payload.push_back(0x55);
    encoded = AshTransport::encodeData(0x03, payload);
    transport.feed(&encoded[0], encoded.size() - 1);
    transport.feed(&cancel, 1);
    transport.feed(&encoded[encoded.size() - 1], 1);
    assert(!transport.popFrame(&frame));
    transport.feed(&overlong[0], overlong.size());
    transport.feed(&encoded[0], encoded.size());
    assert(transport.popFrame(&frame));
    assert(frame.size() == 2 && frame[0] == 0x03 && frame[1] == 0x55);
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
    input.name = "Kitchen \"window\"\\\n";
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
    input.hasLight = true;
    input.lightOn = true;
    input.hasBrightness = true;
    input.brightnessPercent = 63;
    input.hasRemote = true;
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
    Device nondimmable = input;
    nondimmable.ieeeAddress = "0x00124b0000000002";
    nondimmable.networkAddress = 0x1235;
    nondimmable.hasBrightness = false;
    nondimmable.brightnessPercent = -1;
    devices.push_back(nondimmable);
    assert(saveDevices(filename, devices, &error));
    assert(loadDevices(filename, &loaded, &error));
    assert(loaded.size() == 2);
    assert(loaded[0].ieeeAddress == input.ieeeAddress);
    assert(loaded[0].networkAddress == input.networkAddress);
    assert(loaded[0].name == input.name && loaded[0].kind == input.kind);
    assert(loaded[0].hasContact && loaded[0].contactOpen && loaded[0].hasTemperature);
    assert(loaded[0].temperatureCelsius == input.temperatureCelsius);
    assert(loaded[0].hasHumidity && loaded[0].humidityPercent == input.humidityPercent);
    assert(loaded[0].hasOccupancy && loaded[0].occupied);
    assert(loaded[0].hasBattery && loaded[0].batteryPercent == input.batteryPercent);
    assert(loaded[0].hasLight && loaded[0].lightOn && loaded[0].hasBrightness && loaded[0].brightnessPercent == 63 && loaded[0].hasRemote);
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
    assert(loaded[1].ieeeAddress == nondimmable.ieeeAddress && !loaded[1].hasBrightness && loaded[1].brightnessPercent == -1);
    unlink(filename.c_str());
    rmdir(directory.c_str());
}

static void testDeviceLoadRejectsCorruptDuplicatesAndNumbers() {
    std::ostringstream path;
    std::string error;
    std::vector<Device> loaded;
    path << "/tmp/microhad-zigbee-device-corrupt-" << static_cast<long>(getpid());
    assert(ensureDirectory(path.str(), &error));
    const std::string filename = path.str() + "/devices.json";
    {
        std::ofstream output(filename.c_str());
        assert(output);
        output << "{\"ieee_address\":\"ieee-a\",\"network_address\":4660,\"name\":\"first\"}\n";
        output << "{\"ieee_address\":\"ieee-a\",\"network_address\":4661,\"name\":\"duplicate ieee\"}\n";
        output << "{\"ieee_address\":\"ieee-b\",\"network_address\":4660,\"name\":\"duplicate node\"}\n";
        output << "{\"ieee_address\":\"bad-address\",\"network_address\":\"invalid\"}\n";
        output << "{\"ieee_address\":\"bad-range\",\"network_address\":65536}\n";
        output << "{\"ieee_address\":\"bad-brightness\",\"network_address\":4662,\"brightness_percent\":101}\n";
        output << "{\"ieee_address\":\"bad-temperature\",\"network_address\":4666,\"temperature_celsius\":\"not-a-number\"}\n";
        output << "{\"ieee_address\":\"bad-humidity\",\"network_address\":4667,\"humidity_percent\":101}\n";
        output << "not json\n";
        output << "{\"ieee_address\":\"ieee-c\",\"network_address\":4663,\"brightness_percent\":-1}\n";
        output.close(); assert(output);
    }
    assert(loadDevices(filename, &loaded, &error));
    assert(loaded.size() == 2 && loaded[0].ieeeAddress == "ieee-a" && loaded[0].name == "first" &&
           loaded[1].ieeeAddress == "ieee-c" && loaded[1].brightnessPercent == -1);
    Device sentinel; sentinel.ieeeAddress = "unchanged"; loaded.clear(); loaded.push_back(sentinel);
    {
        std::ofstream output(filename.c_str());
        assert(output);
        output << "{\"ieee_address\":\"valid-before-bad\",\"network_address\":4664}\n";
        output << "{\"ieee_address\":\"invalid-descriptor\",\"network_address\":4665,\"endpoint_descriptors\":\"01zz\"}\n";
        output.close(); assert(output);
    }
    error.clear();
    assert(!loadDevices(filename, &loaded, &error) && error == "invalid endpoint descriptor state");
    assert(loaded.size() == 1 && loaded[0].ieeeAddress == "unchanged");
    unlink(filename.c_str());
    loaded.push_back(Device());
    assert(loadDevices(path.str() + "/missing.json", &loaded, &error) && loaded.empty());
    rmdir(path.str().c_str());
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
    assert(gateway.statusJson().find("\"contact_initial_alert_minutes\":10") != std::string::npos);
    assert(gateway.statusJson().find("\"contact_reminder_alert_minutes\":5") != std::string::npos);
    assert(gateway.statusJson().find("\"sensor_refresh_interval_minutes\":60") != std::string::npos);
    assert(gateway.setDeviceSettings("0x00124b0000000004", "Office window", true, 5, true, 30,
                                     true, 20, 7, true, 17, &error));
    assert(gateway.statusJson().find("\"name\":\"Office window\"") != std::string::npos);
    assert(gateway.statusJson().find("\"contact_initial_alert_minutes\":20") != std::string::npos);
    assert(gateway.statusJson().find("\"contact_reminder_alert_minutes\":7") != std::string::npos);
    assert(gateway.statusJson().find("\"sensor_refresh_interval_minutes\":17") != std::string::npos);
    assert(gateway.statusJson().find("\"manufacturer_name\":\"\"") != std::string::npos);
    assert(gateway.statusJson().find("\"model_id\":\"\"") != std::string::npos);
    assert(gateway.setDeviceSettings("0x00124b0000000004", "Office window", true, 5, true, 30,
                                     true, 20, 7, true, 0, &error));
    assert(gateway.statusJson().find("\"sensor_refresh_interval_minutes\":0") != std::string::npos);
    assert(!gateway.setDeviceSettings("0x00124b0000000004", "Office window", true, 30, true, 5,
                                      true, 20, 7, true, 17, &error));
    assert(error == "invalid temperature thresholds");
    assert(!gateway.setDeviceSettings("0x00124b0000000004", "Office window", false, 0, false, 0,
                                      true, 0, 7, true, 0, &error));
    assert(error == "invalid contact notification times");
    assert(!gateway.setDeviceSettings("0xmissing", "Valid name", false, 0, false, 0,
                                      false, 0, 0, false, 0, &error));
    assert(error == "device not found");
    assert(!gateway.setDeviceSettings("0x00124b0000000004", "", false, 0, false, 0,
                                      true, 20, 7, true, 0, &error));
    assert(error == "device name must be between 1 and 64 characters");
    assert(!gateway.setDeviceSettings("0x00124b0000000004", std::string(65, 'x'), false, 0, false, 0,
                                      true, 20, 7, true, 0, &error));
    assert(error == "device name must be between 1 and 64 characters");
    double nan = std::numeric_limits<double>::quiet_NaN();
    assert(!gateway.setDeviceSettings("0x00124b0000000004", "Changed", true, nan, false, 0,
                                      true, 20, 7, true, 0, &error));
    assert(error == "invalid temperature thresholds");
    assert(!gateway.setDeviceSettings("0x00124b0000000004", "Changed", false, 0, false, 0,
                                      true, ULONG_MAX, 7, true, 0, &error));
    assert(error == "invalid contact notification times");
    assert(gateway.statusJson().find("\"name\":\"Office window\"") != std::string::npos);
    assert(gateway.statusJson().find("\"low_temperature_alert\":5") != std::string::npos);
    assert(gateway.statusJson().find("\"high_temperature_alert\":30") != std::string::npos);
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
    assert(loaded.preconfiguredKey == input.preconfiguredKey);
    unlink(filename.c_str()); rmdir(directory.c_str());
}

static void testNetworkPersistenceRejectsInvalidState() {
    std::ostringstream path;
    std::string error;
    NetworkState loaded;
    path << "/tmp/microhad-zigbee-network-invalid-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    std::string filename = directory + "/network.json";
    assert(ensureDirectory(directory, &error));

    loaded.formed = true;
    loaded.panId = 0x1234;
    loaded.networkKey.assign(16, 0xa5);
    assert(loadNetworkState(filename, &loaded, &error));
    assert(!loaded.formed && loaded.panId == 0 && loaded.networkKey.empty());

    const char* invalid[] = {
        "{\"version\":1,\"formed\":true", /* truncated JSON */
        "{\"version\":1,\"formed\":true,\"pan_id\":4660,\"channel\":15,\"extended_pan\":\"00\",\"network_key\":\"0011\",\"preconfigured_key\":\"0011\"}",
        "{\"version\":1,\"formed\":true,\"pan_id\":70000,\"channel\":15,\"extended_pan\":\"1212121212121212\",\"network_key\":\"34343434343434343434343434343434\",\"preconfigured_key\":\"56565656565656565656565656565656\"}",
        "{\"version\":1,\"formed\":true,\"pan_id\":4660,\"channel\":15,\"extended_pan\":\"1212121212121212\",\"network_key\":\"zz343434343434343434343434343434\",\"preconfigured_key\":\"56565656565656565656565656565656\"}",
        "{\"version\":1,\"formed\":true,\"pan_id\":4660,\"channel\":15,\"extended_pan\":\"1212121212121212\",\"network_key\":\"34343434343434343434343434343434\"}"
    };
    for (unsigned long i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        std::ofstream output(filename.c_str());
        output << invalid[i];
        output.close();
        loaded.formed = true;
        loaded.panId = 0x1234;
        loaded.networkKey.assign(16, 0xa5);
        assert(!loadNetworkState(filename, &loaded, &error));
        assert(error == "invalid network state");
        assert(loaded.formed && loaded.panId == 0x1234 && loaded.networkKey.size() == 16 && loaded.networkKey[0] == 0xa5);
    }
    unlink(filename.c_str()); rmdir(directory.c_str());
}

static void testSceneConfigurationPersistence() {
    std::ostringstream path; std::string error; std::vector<SceneGroup> groups, loaded; std::vector<RemoteSceneBinding> bindings, loadedBindings;
    std::vector<NativeRemoteBinding> nativeBindings, loadedNativeBindings;
    path << "/tmp/microhad-zigbee-scenes-test-" << static_cast<long>(getpid());
    assert(ensureDirectory(path.str(), &error));
    SceneGroup group; Scene scene; SceneLightState state; RemoteSceneBinding binding;
    group.id=42; group.name="Kitchen"; group.members.push_back("0x00124b0000000042");
    scene.id=7; scene.name="Dinner"; state.ieeeAddress=group.members[0]; state.on=true; state.brightnessPercent=35; scene.states.push_back(state); group.scenes.push_back(scene);
    Scene secondScene; secondScene.id=8; secondScene.name="Late"; group.scenes.push_back(secondScene);
    groups.push_back(group);
    binding.ieeeAddress="0x00124b0000000099"; binding.action="on"; binding.groupId=42; binding.off=true; binding.sceneIds.push_back(7); binding.sceneIds.push_back(8); bindings.push_back(binding);
    NativeRemoteBinding native; native.ieeeAddress = binding.ieeeAddress; native.groupId = 42; nativeBindings.push_back(native);
    assert(saveSceneConfiguration(path.str()+"/scenes.conf",groups,bindings,nativeBindings,&error));
    assert(loadSceneConfiguration(path.str()+"/scenes.conf",&loaded,&loadedBindings,&loadedNativeBindings,&error));
    assert(loaded.size()==1 && loaded[0].id==42 && loaded[0].name=="Kitchen" && loaded[0].members.size()==1 && loaded[0].members[0]=="0x00124b0000000042");
    assert(loaded[0].scenes.size()==2 && loaded[0].scenes[0].id==7 && loaded[0].scenes[0].name=="Dinner" && loaded[0].scenes[0].states[0].brightnessPercent==35);
    assert(loaded[0].scenes[1].id==8 && loaded[0].scenes[1].name=="Late");
    assert(loadedBindings.size()==1 && loadedBindings[0].ieeeAddress=="0x00124b0000000099" && loadedBindings[0].action=="on" && loadedBindings[0].groupId==42 && loadedBindings[0].off && loadedBindings[0].sceneIds.size()==2 && loadedBindings[0].sceneIds[0]==7 && loadedBindings[0].sceneIds[1]==8);
    assert(loadedNativeBindings.size()==1 && loadedNativeBindings[0].ieeeAddress==native.ieeeAddress && loadedNativeBindings[0].groupId==42);
    unlink((path.str()+"/scenes.conf").c_str()); rmdir(path.str().c_str());
}

static bool saveStorageFixture(int kind, const std::string& filename, std::string* error) {
    if (kind == 0) {
        std::vector<Device> devices;
        return saveDevices(filename, devices, error);
    }
    if (kind == 1) {
        NetworkState network;
        network.formed = true; network.panId = 0x1234; network.channel = 15;
        network.extendedPan.assign(8, 0x12); network.networkKey.assign(16, 0x34);
        network.preconfiguredKey.assign(16, 0x56);
        return saveNetworkState(filename, network, error);
    }
    std::vector<SceneGroup> groups;
    std::vector<RemoteSceneBinding> bindings;
    return saveSceneConfiguration(filename, groups, bindings, error);
}

static void testStorageSaveFailuresPreserveDestinations() {
    std::ostringstream path;
    std::string error, content;
    path << "/tmp/microhad-zigbee-storage-failures-" << static_cast<long>(getpid());
    assert(ensureDirectory(path.str(), &error));
    for (int kind = 0; kind < 3; ++kind) {
        std::ostringstream name;
        name << path.str() << "/save-" << kind;
        std::string target = name.str();
        {
            std::ofstream previous(target.c_str());
            assert(previous); previous << "previous-state\n"; previous.close(); assert(previous);
        }
        assert(mkdir((target + ".tmp").c_str(), 0700) == 0);
        error.clear();
        assert(!saveStorageFixture(kind, target, &error) && !error.empty());
        assert(readTextFile(target, &content) && content == "previous-state\n");
        assert(rmdir((target + ".tmp").c_str()) == 0);

        assert(unlink(target.c_str()) == 0);
        assert(mkdir(target.c_str(), 0700) == 0);
        {
            std::ofstream previous((target + "/keep").c_str());
            assert(previous); previous << "directory-state\n"; previous.close(); assert(previous);
        }
        error.clear();
        assert(!saveStorageFixture(kind, target, &error) && !error.empty());
        assert(readTextFile(target + "/keep", &content) && content == "directory-state\n");
        assert(access((target + ".tmp").c_str(), F_OK) != 0);
        assert(unlink((target + "/keep").c_str()) == 0);
        assert(rmdir(target.c_str()) == 0);
    }
    {
        std::string nested = path.str() + "/missing/child";
        error.clear();
        assert(!ensureDirectory(nested, &error) && error.find("cannot create runtime directory") != std::string::npos);
        std::ofstream file((path.str() + "/not-a-directory").c_str());
        assert(file); file << "file"; file.close();
        error.clear();
        assert(!ensureDirectory(path.str() + "/not-a-directory", &error));
        unlink((path.str() + "/not-a-directory").c_str());
    }
    assert(rmdir(path.str().c_str()) == 0);
}

static void testSceneConfigurationMalformedRecordsAreAtomic() {
    std::ostringstream path; std::string error; std::vector<SceneGroup> groups; std::vector<RemoteSceneBinding> bindings;
    const char* malformed[] = {
        "G\t1\tRoom\nM\t1\tlight\nS\t1\t1\tScene\nT\t1\t1\tlight\tmaybe\t50\n",
        "G\tbad\tRoom\n", "G\t1\tRoom\textra\n", "M\t1\tlight\n", "S\t1\t256\tScene\n",
        "G\t65528\tRoom\n", "S\t1\t0\tScene\n", "T\t1\t1\tlight\t1\t101\n",
        "B\tremote\ton\t1\t2\t1\n", "B\tremote\tflash\t1\t0\t1\n", "X\t1\n",
        "G\t1\tRoom\nG\t1\tDuplicate\n", "G\t1\tRoom\nM\t1\tlight\nM\t1\tlight\n",
        "G\t1\tRoom\nS\t1\t1\tOne\nS\t1\t1\tDuplicate\n",
        "G\t1\tRoom\nS\t1\t1\tOne\nT\t1\t1\tlight\t1\t50\nT\t1\t1\tlight\t0\t20\n"
    };
    path << "/tmp/microhad-zigbee-scenes-malformed-" << static_cast<long>(getpid());
    assert(ensureDirectory(path.str(), &error));
    groups.push_back(SceneGroup()); groups[0].id = 77; groups[0].name = "unchanged";
    bindings.push_back(RemoteSceneBinding()); bindings[0].ieeeAddress = "unchanged";
    for (unsigned long i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        std::ofstream output((path.str() + "/scenes.conf").c_str());
        assert(output); output << malformed[i]; output.close(); assert(output);
        error.clear();
        assert(!loadSceneConfiguration(path.str() + "/scenes.conf", &groups, &bindings, &error));
        assert(error == "invalid scene configuration" && groups.size() == 1 && groups[0].id == 77 &&
               groups[0].name == "unchanged" && bindings.size() == 1 && bindings[0].ieeeAddress == "unchanged");
    }
    {
        std::ofstream output((path.str() + "/scenes.conf").c_str());
        assert(output); output << "# comment only\n\n"; output.close(); assert(output);
    }
    error.clear();
    assert(loadSceneConfiguration(path.str() + "/scenes.conf", &groups, &bindings, &error));
    assert(groups.empty() && bindings.empty());
    unlink((path.str()+"/scenes.conf").c_str()); rmdir(path.str().c_str());
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
    assert(device.hasTemperature && device.temperatureCelsius == 23.3 && device.lastSeen == 123);
    humidity.push_back(0xb8); humidity.push_back(0x13); /* 50.48 % */
    assert(applyZclAttribute(&device, 0x0405, 0x0000, humidity, 123));
    assert(device.hasHumidity && device.humidityPercent == 50.48 && device.lastSeen == 123);
    xiaomi.push_back(8); /* length */
    xiaomi.push_back(0x64); xiaomi.push_back(0x29); xiaomi.push_back(0xa8); xiaomi.push_back(0x08); /* 22.16 C */
    xiaomi.push_back(0x65); xiaomi.push_back(0x21); xiaomi.push_back(0xb8); xiaomi.push_back(0x13); /* 50.48 % */
    device = Device();
    assert(applyZclAttribute(&device, 0x0000, 0xff01, xiaomi, 124));
    assert(device.hasTemperature && device.temperatureCelsius == 22.16);
    assert(device.hasHumidity && device.humidityPercent == 50.48);
    {
        std::vector<unsigned char> temperatureOnly;
        std::vector<unsigned char> humidityOnly;
        std::vector<unsigned char> unknown;
        temperatureOnly.push_back(4); temperatureOnly.push_back(0x64); temperatureOnly.push_back(0x29);
        temperatureOnly.push_back(0xa8); temperatureOnly.push_back(0x08);
        humidityOnly.push_back(4); humidityOnly.push_back(0x65); humidityOnly.push_back(0x21);
        humidityOnly.push_back(0xb8); humidityOnly.push_back(0x13);
        unknown.push_back(4); unknown.push_back(0x99); unknown.push_back(0x21); unknown.push_back(1); unknown.push_back(0);
        device = Device();
        assert(applyZclAttribute(&device, 0x0000, 0xff01, temperatureOnly, 125));
        assert(device.hasTemperature && !device.hasHumidity && device.temperatureCelsius == 22.16);
        device = Device();
        assert(applyZclAttribute(&device, 0x0000, 0xff01, humidityOnly, 126));
        assert(!device.hasTemperature && device.hasHumidity && device.humidityPercent == 50.48);
        device = Device();
        assert(!applyZclAttribute(&device, 0x0000, 0xff01, unknown, 127));
        assert(!device.hasTemperature && !device.hasHumidity && device.lastSeen == 0);
    }
    occupancy.push_back(1);
    assert(applyZclAttribute(&device, 0x0406, 0x0000, occupancy, 123));
    assert(device.hasOccupancy && device.occupied && device.lastSeen == 123);
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
    assert(device.hasBattery && device.batteryPercent == 85 && device.lastSeen == 124);
    zone.push_back(1); zone.push_back(0);
    assert(applyIasZoneStatus(&device, zone, 125));
    assert(device.hasContact && device.contactOpen && device.lastSeen == 125);
    zone[0] = 0;
    assert(applyIasZoneStatus(&device, zone, 126));
    assert(device.hasContact && !device.contactOpen && device.lastSeen == 126);

    device = Device();
    std::vector<unsigned char> on(1, 1);
    std::vector<unsigned char> level(1, 127);
    assert(applyZclAttribute(&device, 0x0006, 0x0000, on, 126));
    assert(device.hasLight && device.lightOn && device.lastSeen == 126);
    assert(applyZclAttribute(&device, 0x0008, 0x0000, level, 127));
    assert(device.hasBrightness && device.brightnessPercent == 50 && device.lastSeen == 127);
    assert(isLightAttribute(0x0006, 0x0000) && isLightAttribute(0x0008, 0x0000));
    assert(!isLightAttribute(0x0008, 0x0001));
}

static void testZclRejectsMalformedAttributes() {
    Device device;
    std::vector<unsigned char> empty;
    std::vector<unsigned char> one(1, 0);
    std::vector<unsigned char> two(2, 0);
    std::vector<unsigned char> three(3, 0);
    std::vector<unsigned char> invalidHumidity;
    std::vector<unsigned char> invalidBattery(1, 201);
    std::vector<unsigned char> invalidOn(1, 2);
    std::vector<unsigned char> invalidLevel(1, 255);
    std::vector<unsigned char> wrongType;
    const unsigned short twoByteClusters[] = {0x0402, 0x0405, 0x0500};
    const unsigned short twoByteAttributes[] = {0x0000, 0x0000, 0x0002};
    const unsigned short oneByteClusters[] = {0x0406, 0x0001, 0x0006, 0x0008};
    const unsigned short oneByteAttributes[] = {0x0000, 0x0021, 0x0000, 0x0000};
    for (unsigned long i = 0; i < sizeof(twoByteClusters) / sizeof(twoByteClusters[0]); ++i) {
        assert(!applyZclAttribute(&device, twoByteClusters[i], twoByteAttributes[i], empty, 10));
        assert(!applyZclAttribute(&device, twoByteClusters[i], twoByteAttributes[i], one, 10));
        assert(!applyZclAttribute(&device, twoByteClusters[i], twoByteAttributes[i], three, 10));
    }
    for (unsigned long i = 0; i < sizeof(oneByteClusters) / sizeof(oneByteClusters[0]); ++i) {
        assert(!applyZclAttribute(&device, oneByteClusters[i], oneByteAttributes[i], empty, 10));
        assert(!applyZclAttribute(&device, oneByteClusters[i], oneByteAttributes[i], two, 10));
    }
    invalidHumidity.push_back(0x11); invalidHumidity.push_back(0x27); /* 100.01% */
    assert(!applyZclAttribute(&device, 0x0405, 0x0000, invalidHumidity, 10));
    assert(!applyZclAttribute(&device, 0x0001, 0x0021, invalidBattery, 10));
    assert(!applyZclAttribute(&device, 0x0006, 0x0000, invalidOn, 10));
    assert(!applyZclAttribute(&device, 0x0008, 0x0000, invalidLevel, 10));
    assert(!applyZclAttribute(&device, 0x9999, 0x0000, one, 10));
    assert(!applyIasZoneStatus(&device, empty, 10));
    assert(!applyIasZoneStatus(&device, three, 10));
    wrongType.push_back(0); wrongType.push_back(0); wrongType.push_back(0); /* attribute + success */
    wrongType.push_back(0x21); wrongType.push_back(0); wrongType.push_back(0); /* unsigned instead of signed temperature */
    assert(!applyZclReadAttributeResponse(&device, 0x0402, wrongType, 10));
    assert(!device.hasTemperature && !device.hasHumidity && !device.hasOccupancy && !device.hasBattery &&
           !device.hasContact && !device.hasLight && device.lastSeen == 0);
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
    assert(gateway.statusJson().find("\"endpoint_descriptors\":[{\"endpoint\":1,\"profile_id\":260,\"device_id\":770,\"device_version\":1,\"input_clusters\":[0,1,1026,1029],\"output_clusters\":[3]}") != std::string::npos);
    assert(GatewayTestPeer::operationCount(gateway, 0x1234) == 10);
    assert(GatewayTestPeer::operationType(gateway, 0x1234, 0) == 3); /* InterviewBind */
    assert(GatewayTestPeer::operationCluster(gateway, 0x1234, 0) == 0x0402);
    assert(GatewayTestPeer::operationCluster(gateway, 0x1234, 1) == 0x0405);
    assert(GatewayTestPeer::operationCluster(gateway, 0x1234, 2) == 0x0001);
    {
        EndpointDescriptor remote;
        Gateway remoteGateway("/dev/null", 115200, "/tmp", "", "", "");
        remote.endpoint = 1;
        remote.outputClusters.push_back(0x0006);
        remote.outputClusters.push_back(0x0008);
        GatewayTestPeer::buildConfiguration(&remoteGateway, 0x1235, "0x00124b0000000012", remote);
        assert(GatewayTestPeer::operationCount(remoteGateway, 0x1235) == 2);
        assert(GatewayTestPeer::operationType(remoteGateway, 0x1235, 0) == 3);
        assert(GatewayTestPeer::operationCluster(remoteGateway, 0x1235, 0) == 0x0006);
        assert(GatewayTestPeer::operationType(remoteGateway, 0x1235, 1) == 3);
        assert(GatewayTestPeer::operationCluster(remoteGateway, 0x1235, 1) == 0x0008);
    }
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

static void testInterviewRetryDeadline() {
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    assert(GatewayTestPeer::interviewRetryContributesDeadline(&gateway, 0x2345));
}

static void testGatewayViewIsImmutable() {
    unlink("/tmp/microhad-zigbee-native.log");
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayTestPeer::completeViewSchema(&gateway);
    GatewayView first = gateway.captureView();
    std::string status = first.statusJson();
    const char* fields[] = {"\"online\":", "\"network_forming\":", "\"network_formed\":", "\"serial_rx_bytes\":",
                            "\"ash_rx_frames\":", "\"joining_until\":", "\"joining_preparing\":", "\"notifications_enabled\":",
                            "\"devices\":[", "\"scene_groups\":[", "\"scene_bindings\":[", "\"endpoint_descriptors\":["};
    for (unsigned long i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) assert(status.find(fields[i]) != std::string::npos);
    assert(status.find("Kitchen \\\"window\\\"\\n") != std::string::npos);
    assert(first.logsJson().find("view control\\n\\u0001") != std::string::npos);
    std::vector<unsigned char> temperature;
    temperature.push_back(0x10); temperature.push_back(0x27); /* 100.00 C */
    assert(gateway.takeViewDirty());
    assert(gateway.applyAttributeReport("0x00124b00000000fe", 0x0402, 0x0000, temperature));
    GatewayView second = gateway.captureView();
    assert(first.statusJson().find("0x00124b00000000fe") == std::string::npos);
    assert(second.statusJson().find("0x00124b00000000fe") != std::string::npos);
    assert(gateway.takeViewDirty());
    assert(!gateway.takeViewDirty());
    unlink("/tmp/microhad-zigbee-native.log");
}

static void testRamLog() {
    std::string error;
    unlink("/tmp/microhad-zigbee-native.log");
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    assert(!gateway.setJoining(300, &error));
    assert(error == "Zigbee coordinator is offline");
    assert(gateway.logsJson().find("Pairing request rejected: coordinator offline") != std::string::npos);
    GatewayTestPeer::logRingAndSerialization();
    unlink("/tmp/microhad-zigbee-native.log");
}

static void testFrameLogs() {
    unlink("/tmp/microhad-zigbee-native.log");
    assert(GatewayTestPeer::frameLogsContainDecodedDetails());
    unlink("/tmp/microhad-zigbee-native.log");
}

static void testRemoteKeyEvents() {
    unlink("/tmp/microhad-zigbee-native.log");
    assert(GatewayTestPeer::remoteKeyEventsAreLogged());
    GatewayTestPeer::tradfriFirmwareBindingAndOta();
    assert(GatewayTestPeer::nativeRemoteBindingFlow());
    assert(GatewayTestPeer::nativeIkeaFirmwareModes());
    unlink("/tmp/microhad-zigbee-native.log");
}

static void testStyrbarArrowEvents() {
    unlink("/tmp/microhad-zigbee-native.log");
    assert(GatewayTestPeer::styrbarArrowEventsAreLogged());
    assert(GatewayTestPeer::remoteBindingsAndIgnoredEvents());
    unlink("/tmp/microhad-zigbee-native.log");
}

static void testSceneConfigurationRejectsDuplicateStates() {
    assert(GatewayTestPeer::rejectsDuplicateSceneStates());
}

static void testSceneConfigurationRejectsInvalidReferences() {
    assert(GatewayTestPeer::rejectsInvalidSceneCrossReferences());
}

static void testSaveTriggeredProvisioning() {
    assert(GatewayTestPeer::saveTriggeredProvisioning());
}

static void testSceneBusyDoesNotAdvanceSelection() {
    assert(GatewayTestPeer::sceneBusyDoesNotAdvanceSelection());
    assert(GatewayTestPeer::sceneRuntimePersistence());
    assert(GatewayTestPeer::nativeAndFallbackSceneOutcomes());
    GatewayTestPeer::sceneBusyUnicastFallback();
}

static void testPendingSceneIgnoresRemoteAction() {
    assert(GatewayTestPeer::pendingSceneIgnoresRemoteAction());
}

static void testContactNotificationIsDeferred() {
    std::ostringstream path;
    std::string error;
    std::vector<unsigned char> zone;
    std::vector<unsigned char> warm;
    std::vector<unsigned char> cold;
    path << "/tmp/microhad-zigbee-alert-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    NotificationWorker worker(directory, "", "", "");
    assert(worker.setEnabled(false, &error));
    Gateway gateway("/dev/null", 115200, directory, "", "", "", &worker);
    zone.push_back(1); zone.push_back(0);
    assert(gateway.applyIasZoneStatus("0x00124b0000000003", zone));
    assert(gateway.logsJson().find("Contact opened") == std::string::npos);
    assert(GatewayTestPeer::contactAlertIsCancelled(&gateway, "0x00124b0000000004"));
    warm.push_back(0xd0); warm.push_back(0x07); /* 20.00 C */
    cold.push_back(0xf4); cold.push_back(0x01); /* 5.00 C */
    assert(gateway.applyAttributeReport("0x00124b0000000005", 0x0402, 0x0000, warm));
    assert(gateway.setDeviceSettings("0x00124b0000000005", "Cold room", true, 10, false, 0,
                                     false, 0, 0, false, 0, &error));
    assert(gateway.applyAttributeReport("0x00124b0000000005", 0x0402, 0x0000, cold));
    assert(!gateway.sendTestNotification(&error));
    assert(error == "push notifications are disabled");
    assert(gateway.logsJson().find("Notification ") == std::string::npos);
    unlink((directory + "/notification-outbox.ndjson").c_str());
    unlink((directory + "/devices.json").c_str());
    rmdir(directory.c_str());
}

static void testTemperatureThresholdAlerts() {
    std::ostringstream path;
    std::string error;
    std::vector<unsigned char> temperature;
    path << "/tmp/microhad-zigbee-temperature-alert-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    NotificationWorker worker(directory, "", "", "");
    assert(worker.start(&error));
    Gateway gateway("/dev/null", 115200, directory, "", "", "", &worker);
    temperature.push_back(0xd0); temperature.push_back(0x07); /* 20.00 C */
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(gateway.setDeviceSettings("0x00124b0000000006", "Utility", true, 10, true, 30,
                                     false, 0, 0, false, 0, &error));
    temperature[0] = 0x1c; temperature[1] = 0x0c; /* 31.00 C */
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature high") == 1);
    temperature[0] = 0x80; temperature[1] = 0x0c; /* 32.00 C */
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature high") == 1);
    temperature[0] = 0xd0; temperature[1] = 0x07; /* back inside */
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    temperature[0] = 0x1c; temperature[1] = 0x0c;
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature high") == 2);
    temperature[0] = 0x84; temperature[1] = 0x03; /* 9.00 C */
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature low") == 1);
    temperature[0] = 0x20; temperature[1] = 0x03; /* 8.00 C */
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature low") == 1);
    assert(gateway.setDeviceSettings("0x00124b0000000006", "Utility", false, 0, false, 0,
                                     false, 0, 0, false, 0, &error));
    temperature[0] = 0x1c; temperature[1] = 0x0c;
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature high") == 2);
    {
        std::map<std::string, DeviceValueAlert> alerts;
        DeviceValueAlert temperatureAlert;
        temperatureAlert.hasLow = true; temperatureAlert.low = 10;
        temperatureAlert.hasHigh = true; temperatureAlert.high = 30;
        temperatureAlert.hysteresis = 2;
        alerts["temperature"] = temperatureAlert;
        assert(gateway.setDeviceValueAlerts("0x00124b0000000006", alerts, &error));
        assert(gateway.statusJson().find("\"hysteresis\":2") != std::string::npos);
        temperature[0] = 0x54; temperature[1] = 0x0b; /* 29.00 C */
        assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
        temperature[0] = 0xb8; temperature[1] = 0x0b; /* 30.00 C */
        assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
        assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature high") == 3);
        temperature[0] = 0x84; temperature[1] = 0x03; /* 9.00 C */
        assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
        assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature low") == 2);
        temperature[0] = 0xe8; temperature[1] = 0x03; /* 10.00 C, still inside hysteresis band */
        assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
        assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature low") == 2);
        temperature[0] = 0xb0; temperature[1] = 0x04; /* 12.00 C, clear low state */
        assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
        temperature[0] = 0xe8; temperature[1] = 0x03;
        assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
        assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature low") == 3);
    }
    worker.stop();
    unlink((directory + "/devices.json").c_str());
    rmdir(directory.c_str());
}

static void testContactAlertReminders() {
    std::ostringstream path;
    std::string error;
    path << "/tmp/microhad-zigbee-contact-alert-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    NotificationWorker worker(directory, "", "", "");
    assert(worker.start(&error));
    Gateway gateway("/dev/null", 115200, directory, "", "", "", &worker);
    assert(GatewayTestPeer::contactAlertDeadlines(&gateway, "0x00124b0000000007"));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Window open") == 2);
    worker.stop();
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
    {
        std::string persisted;
        assert(readTextFile(directory + "/notifications.json", &persisted));
        assert(persisted == "{\"enabled\":true}\n");
        assert(access((directory + "/notifications.json.tmp").c_str(), F_OK) != 0);
    }
    {
        NotificationWorker worker(directory, "", "", "");
        assert(worker.enabled());
    }
    unlink((directory + "/notifications.json").c_str());
    rmdir(directory.c_str());
}

static void testPresenceBasedPushRouting() {
    std::vector<NotificationWorker::RouteClient> clients;
    NotificationWorker::RouteClient client;
    client.id = "phone-home"; client.presenceSensorId = "sensor-home"; client.enabled = true; clients.push_back(client);
    client.id = "browser-away"; client.presenceSensorId = "sensor-away"; clients.push_back(client);
    client.id = "phone-unassigned"; client.presenceSensorId = ""; clients.push_back(client);
    client.id = "disabled-home"; client.presenceSensorId = "sensor-home"; client.enabled = false; clients.push_back(client);
    std::vector<std::string> present, selected;
    present.push_back("sensor-home");
    selected = NotificationWorker::selectRecipients(clients, present);
    assert(selected.size() == 2 && selected[0] == "phone-home" && selected[1] == "phone-unassigned");
    present.clear();
    selected = NotificationWorker::selectRecipients(clients, present);
    assert(selected.size() == 3 && selected[0] == "phone-home" && selected[1] == "browser-away" && selected[2] == "phone-unassigned");
    present.push_back("unknown-sensor");
    selected = NotificationWorker::selectRecipients(clients, present);
    assert(selected.size() == 3);
    present.clear(); present.push_back("sensor-away");
    selected = NotificationWorker::selectRecipients(clients, present);
    assert(selected.size() == 2 && selected[0] == "browser-away" && selected[1] == "phone-unassigned");
}

static void testPushDeviceRegistry() {
    char directoryName[] = "/tmp/microhad-zigbee-push-devices-XXXXXX";
    char* directory = mkdtemp(directoryName);
    assert(directory != 0);
    const std::string id = "11111111-1111-4111-8111-111111111111";
    std::string error, before, after;
    struct stat firstStat, secondStat;
    {
        NotificationWorker worker(directory, "", "", "");
        assert(!worker.registerDevice("wrong", "Pixel", "token", &error));
        assert(worker.registerDevice(id, "Pixel 7", "token-one", &error));
        assert(readTextFile(std::string(directory) + "/push-devices.ndjson", &before));
        assert(stat((std::string(directory) + "/push-devices.ndjson").c_str(), &firstStat) == 0);
        assert((firstStat.st_mode & 0777) == 0600);
        assert(worker.registerDevice(id, "Pixel 7", "token-one", &error));
        assert(readTextFile(std::string(directory) + "/push-devices.ndjson", &after) && before == after);
        assert(stat((std::string(directory) + "/push-devices.ndjson").c_str(), &secondStat) == 0);
        assert(firstStat.st_ino == secondStat.st_ino);
        assert(worker.updateDevice(id, "Alice's phone", true, "sensor-hall", &error));
        assert(worker.updateDevice(id, "Alice's phone", false, &error));
        assert(worker.registerDevice(id, "Pixel 8", "token-two", &error));
        assert(worker.devicesJson().find("Alice's phone") != std::string::npos);
        assert(worker.devicesJson().find("\"enabled\":false") != std::string::npos);
        assert(worker.devicesJson().find("\"presence_sensor_id\":\"sensor-hall\"") != std::string::npos);
        assert(worker.devicesJson().find("token-two") == std::string::npos);
    }
    {
        NotificationWorker worker(directory, "", "", "");
        assert(worker.devicesJson().find("Alice's phone") != std::string::npos);
        assert(worker.devicesJson().find("Pixel 8") != std::string::npos);
        assert(worker.devicesJson().find("\"enabled\":false") != std::string::npos);
    }
    unlink((std::string(directory) + "/push-devices.ndjson").c_str());
    rmdir(directory);
}

class BlockingNotificationDelivery : public NotificationWorker::DeliveryHook {
public:
    BlockingNotificationDelivery() : entered_(false), released_(false), delivered_(0) {
        pthread_mutex_init(&mutex_, 0);
        pthread_cond_init(&changed_, 0);
    }
    ~BlockingNotificationDelivery() {
        pthread_cond_destroy(&changed_);
        pthread_mutex_destroy(&mutex_);
    }
    void deliver(const std::string&, const std::string&) {
        pthread_mutex_lock(&mutex_);
        ++delivered_;
        if (delivered_ == 1) {
            entered_ = true;
            pthread_cond_broadcast(&changed_);
            while (!released_) pthread_cond_wait(&changed_, &mutex_);
        }
        pthread_mutex_unlock(&mutex_);
    }
    bool waitUntilEntered() {
        struct timeval now;
        struct timespec deadline;
        gettimeofday(&now, 0);
        deadline.tv_sec = now.tv_sec + 2;
        deadline.tv_nsec = now.tv_usec * 1000;
        pthread_mutex_lock(&mutex_);
        while (!entered_) {
            int result = pthread_cond_timedwait(&changed_, &mutex_, &deadline);
            if (result != 0) { pthread_mutex_unlock(&mutex_); return false; }
        }
        pthread_mutex_unlock(&mutex_);
        return true;
    }
    void release() {
        pthread_mutex_lock(&mutex_);
        released_ = true;
        pthread_cond_broadcast(&changed_);
        pthread_mutex_unlock(&mutex_);
    }
    int delivered() {
        pthread_mutex_lock(&mutex_);
        int value = delivered_;
        pthread_mutex_unlock(&mutex_);
        return value;
    }
private:
    pthread_mutex_t mutex_;
    pthread_cond_t changed_;
    bool entered_, released_;
    int delivered_;
};

static void testNotificationWorkerQueueLifecycle() {
    char directoryName[] = "/tmp/microhad-zigbee-worker-XXXXXX";
    char* directory = mkdtemp(directoryName);
    std::string error;
    unsigned int i;
    assert(directory != 0);
    BlockingNotificationDelivery delivery;
    NotificationWorker worker(directory, "", "", "", &delivery);
    assert(worker.start(&error));
    assert(!worker.start(&error) && error == "notification worker already started");
    assert(worker.enqueue("first", "body", &error));
    assert(delivery.waitUntilEntered());
    for (i = 0; i < 32; ++i) assert(worker.enqueue("queued", "body", &error));
    assert(!worker.enqueue("overflow", "body", &error) && error == "notification queue is full");
    delivery.release();
    worker.stop();
    assert(delivery.delivered() == 33);
    worker.stop();
    assert(worker.start(&error));
    assert(worker.setEnabled(false, &error));
    assert(!worker.enqueue("disabled", "body", &error) && error == "push notifications are disabled");
    worker.stop();
    assert(worker.start(&error));
    worker.stop();
    unlink((std::string(directory) + "/notifications.json").c_str());
    rmdir(directory);
    fprintf(stderr, "unit: notification worker lifecycle and bounded queue\n");
}

static void testNotificationAuditDoesNotWriteRuntimeFlash() {
    std::ostringstream path;
    std::string error;
    path << "/tmp/microhad-zigbee-notification-audit-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    {
        NotificationWorker worker(directory, "", "", "");
        Gateway gateway("/dev/null", 115200, directory, "", "", "", &worker);
        assert(worker.start(&error));
        assert(gateway.sendTestNotification(&error));
        assert(access((directory + "/notification-outbox.ndjson").c_str(), F_OK) != 0);
    }
    rmdir(directory.c_str());
}

static std::string pushSubscription(const std::string& endpoint, char publicKeyTail) {
    std::string publicKey(65, '\0');
    std::string auth(16, '\0');
    publicKey[0] = 4;
    publicKey[64] = publicKeyTail;
    auth[15] = publicKeyTail;
    return "{\"endpoint\":\"" + endpoint + "\",\"keys\":{\"p256dh\":\"" + base64UrlEncode(publicKey) +
           "\",\"auth\":\"" + base64UrlEncode(auth) + "\"}}";
}

static void testBrowserPushSubscriptionBounds() {
    std::ostringstream path;
    std::string error, content;
    path << "/tmp/microhad-zigbee-push-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    WebPushStore store(directory);
    assert(store.addSubscription(pushSubscription("https://push.example.test/a", 1), &error));
    assert(store.addSubscription(pushSubscription("https://push.example.test/a", 2), &error));
    assert(readTextFile(directory + "/web-push-subscriptions.ndjson", &content));
    assert(content.find("https://push.example.test/a") != std::string::npos && content.find("\n") == content.size() - 1);
    assert(content.find("https://push.example.test/a") == content.rfind("https://push.example.test/a"));
    assert(!store.addSubscription(pushSubscription("http://push.example.test/a", 3), &error));
    for (int i = 0; i < 31; ++i) {
        std::ostringstream endpoint;
        endpoint << "https://push.example.test/" << i;
        assert(store.addSubscription(pushSubscription(endpoint.str(), static_cast<char>(i + 4)), &error));
    }
    assert(!store.addSubscription(pushSubscription("https://push.example.test/overflow", 40), &error));
    assert(error == "Browser Push subscription limit reached");
    unlink((directory + "/web-push-subscriptions.ndjson").c_str());
    rmdir(directory.c_str());
}

static void testFcmMessageConstruction() {
    FirebaseServiceAccount account;
    std::string error;
    std::string json = "{\"project_id\":\"project\",\"client_email\":\"x@example.test\",\"private_key\":\"-----BEGIN PRIVATE KEY-----\\nKEY\\n-----END PRIVATE KEY-----\\n\"}";
    assert(parseFirebaseServiceAccount(json, &account, &error));
    assert(account.privateKeyPem == "-----BEGIN PRIVATE KEY-----\nKEY\n-----END PRIVATE KEY-----\n");
    assert(base64UrlEncode("foo") == "Zm9v");
    assert(base64UrlEncode("f") == "Zg");
    std::string decoded;
    assert(base64UrlDecode("Zm9v", &decoded) && decoded == "foo");
    assert(base64UrlDecode("Zg", &decoded) && decoded == "f");
    assert(makeFcmMessage("zigbee_alerts", "Window open", "Kitchen").find("\"topic\":\"zigbee_alerts\"") != std::string::npos);
}

static void testSharedJsonUtilities() {
    std::string value;
    long number = 0;
    assert(jsonStringField("{ \"value\" : \"quoted \\u005c\\\" value\\n\" }", "value", &value));
    assert(value == "quoted \\\" value\n");
    assert(jsonStringField("{\"value\":\"\\u20ac \\ud83d\\ude03\"}", "value", &value));
    assert(value == "\342\202\254 \360\237\230\203");
    assert(!jsonStringField("{\"value\":\"\\ud800\"}", "value", &value));
    assert(jsonLongField("{\"number\": 42}", "number", &number) && number == 42);
    assert(!jsonLongField("{\"number\": 42oops}", "number", &number));
    assert(!jsonStringField("{\"value\":\"first\",\"value\":\"second\"}", "value", &value));
    assert(!jsonStringField("{\"other\":\"value\"}", "value", &value));
    assert(!jsonStringField("{\"value\":\"value\"} trailing", "value", &value));
    assert(!jsonLongField("{\"number\":42} trailing", "number", &number));
    assert(jsonEscape("a\\b\"c\n\001") == "a\\\\b\\\"c\\n\\u0001");
}

void runUnitTests() {
    GatewayTestPeer::closingPermitReplyTimeout();
    GatewayTestPeer::refreshDiscoversMissingEndpoints();
    GatewayTestPeer::reviewAndTransportRegressions();
    GatewayTestPeer::ezspUpgradeCompatibility();
    GatewayTestPeer::networkJoinWindowDoesNotClearNetworkState();
    GatewayTestPeer::lightCommandEncoding();
    GatewayTestPeer::identifyCommandEncoding();
    testAshRoundTrip();
    testAshRejectsBadCrc();
    testAshFragmentationAndFrameOrdering();
    testAshRejectsMalformedAndOverlongInput();
    testDevicePersistence();
    testDeviceLoadRejectsCorruptDuplicatesAndNumbers();
    testDeviceSettings();
    testNetworkPersistence();
    testNetworkPersistenceRejectsInvalidState();
    testSceneConfigurationPersistence();
    testSceneConfigurationMalformedRecordsAreAtomic();
    testStorageSaveFailuresPreserveDestinations();
    testGenericCapabilities();
    testZclRejectsMalformedAttributes();
    testConfigureReportingEncoding();
    testInterviewDescriptorAndPlanning();
    testJoinStatusJson();
    testInterviewRetryDeadline();
    testGatewayViewIsImmutable();
    testRamLog();
    testFrameLogs();
    testRemoteKeyEvents();
    testStyrbarArrowEvents();
    testSceneConfigurationRejectsDuplicateStates();
    testSceneConfigurationRejectsInvalidReferences();
    testSaveTriggeredProvisioning();
    testSceneBusyDoesNotAdvanceSelection();
    testPendingSceneIgnoresRemoteAction();
    testContactNotificationIsDeferred();
    testTemperatureThresholdAlerts();
    testContactAlertReminders();
    testNotificationTogglePersistence();
    testPresenceBasedPushRouting();
    testPushDeviceRegistry();
    testNotificationWorkerQueueLifecycle();
    testNotificationAuditDoesNotWriteRuntimeFlash();
    testBrowserPushSubscriptionBounds();
    testFcmMessageConstruction();
    testSharedJsonUtilities();
}
