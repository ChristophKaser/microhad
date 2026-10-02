#ifndef MICROHAD_ZIGBEE_GATEWAY_INTEGRATION_PEER_H
#define MICROHAD_ZIGBEE_GATEWAY_INTEGRATION_PEER_H

#include "gateway.h"
#include "storage.h"

#include <cassert>
#include <cstdio>
#include <ctime>
#include <fcntl.h>

class GatewayIntegrationTestPeer {
public:
    static void injectEzspFrame(Gateway* gateway, const std::vector<unsigned char>& frame, unsigned char version) {
        gateway->ezspVersion_ = version;
        gateway->handleEzspFrame(frame);
    }
    static void requestSecurityDiagnostics(Gateway* gateway) { gateway->requestSecurityDiagnostics("integration"); }
    static unsigned char securityDiagnosticSequence(const Gateway& gateway, unsigned char value) {
        for (std::map<unsigned char, Gateway::SecurityDiagnosticQuery>::const_iterator it =
                 gateway.securityDiagnosticQueries_.begin(); it != gateway.securityDiagnosticQueries_.end(); ++it)
            if (it->second.value == value) return it->first;
        assert(false); return 0;
    }
    static unsigned long securityDiagnosticCount(const Gateway& gateway) { return gateway.securityDiagnosticQueries_.size(); }
    static void resetRadio(Gateway* gateway) { gateway->sendAshReset(); }
    static void attachSerial(Gateway* gateway, int fd) {
        assert(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
        gateway->serialFd_ = fd;
    }
    static bool online(const Gateway& gateway) { return gateway.online_; }
    static unsigned long queueSerialBytes(Gateway* gateway, const std::vector<unsigned char>& bytes) {
        gateway->serialPendingBytes_ = bytes;
        gateway->flushSerialOutput();
        return gateway->serialPendingBytes_.size();
    }
    static unsigned long pendingSerialBytes(const Gateway& gateway) { return gateway.serialPendingBytes_.size(); }
    static void forceSerialWrite(Gateway* gateway) { gateway->serialPendingBytes_.push_back(0x55); gateway->flushSerialOutput(); }
    static void flushSerialOutput(Gateway* gateway) { gateway->flushSerialOutput(); }
    static void queueEzspCommand(Gateway* gateway, unsigned short command) {
        gateway->sendEzspCommand(command, std::vector<unsigned char>());
    }
    static void setAshSendSequence(Gateway* gateway, unsigned char sequence) { gateway->ashSendSequence_ = sequence & 7; }
    static void expireAshAck(Gateway* gateway) { gateway->ashAckDeadline_ = time(0) - 1; }
    static bool prepareFreshNetwork(Gateway* gateway, std::string* error) {
        gateway->online_ = true;
        gateway->networkInitRequested_ = true;
        gateway->coordinatorInitializationStep_ = 0;
        gateway->networkFormed_ = false;
        gateway->ezspVersion_ = 8;
        return gateway->createFreshNetwork(error);
    }
    static void setNetworkReady(Gateway* gateway, bool formed) {
        gateway->online_ = true;
        gateway->ezspVersion_ = 8;
        gateway->networkInitRequested_ = true;
        gateway->coordinatorInitializationStep_ = 0;
        gateway->networkFormed_ = formed;
    }
    static void setNetworkRandomSource(Gateway* gateway, const std::string& path) {
        gateway->networkRandomSource_ = path;
    }
    static bool networkForming(const Gateway& gateway) { return gateway.networkForming_; }
    static void expireNetworkFormation(Gateway* gateway) { gateway->networkFormationDeadline_ = time(0) - 1; }
    static bool prepareJoining(Gateway* gateway, unsigned int seconds, std::string* error) {
        gateway->online_ = true;
        gateway->networkFormed_ = true;
        gateway->ezspVersion_ = 8;
        gateway->coordinatorInitializationStep_ = 0;
        return gateway->setJoining(seconds, error);
    }
    static void expireJoinPreparation(Gateway* gateway) { gateway->joinPreparationDeadline_ = time(0) - 1; }
    static void setJoinDeadlines(Gateway* gateway, long until, long renewAt) {
        gateway->joinUntil_ = until;
        gateway->joinPermitRenewAt_ = renewAt;
    }
    static void addIasDevice(Gateway* gateway, unsigned short node, const std::string& ieee, int zoneId) {
        gateway->online_ = true;
        gateway->networkFormed_ = true;
        gateway->ezspVersion_ = 8;
        Device device;
        EndpointDescriptor endpoint;
        device.ieeeAddress = ieee;
        device.networkAddress = node;
        device.iasZoneId = zoneId;
        endpoint.endpoint = 1;
        endpoint.inputClusters.push_back(0x0500);
        device.endpoints.push_back(endpoint);
        gateway->devices_[ieee] = device;
        gateway->nodeAddresses_[node] = ieee;
    }
    static void addMeasurementDevice(Gateway* gateway, unsigned short node, const std::string& ieee) {
        gateway->online_ = true;
        gateway->networkFormed_ = true;
        gateway->ezspVersion_ = 8;
        Device device;
        EndpointDescriptor endpoint;
        device.ieeeAddress = ieee;
        device.networkAddress = node;
        device.hasTemperature = true;
        device.temperatureCelsius = 15.0;
        gateway->nextSensorRefreshAt_[node] = time(0) + 300;
        endpoint.endpoint = 1;
        endpoint.inputClusters.push_back(0x0402);
        device.endpoints.push_back(endpoint);
        gateway->devices_[ieee] = device;
        gateway->nodeAddresses_[node] = ieee;
    }
    static void addContactDevice(Gateway* gateway, unsigned short node, const std::string& ieee) {
        gateway->online_ = true;
        gateway->networkFormed_ = true;
        gateway->ezspVersion_ = 8;
        Device device;
        EndpointDescriptor endpoint;
        device.ieeeAddress = ieee;
        device.networkAddress = node;
        device.hasContact = true;
        endpoint.endpoint = 1;
        endpoint.inputClusters.push_back(0x0500);
        device.endpoints.push_back(endpoint);
        gateway->devices_[ieee] = device;
        gateway->nodeAddresses_[node] = ieee;
    }
    static void addRemoteDevice(Gateway* gateway, unsigned short node, const std::string& ieee) {
        gateway->online_ = true;
        gateway->networkFormed_ = true;
        gateway->ezspVersion_ = 8;
        Device device;
        EndpointDescriptor endpoint;
        device.ieeeAddress = ieee;
        device.networkAddress = node;
        device.hasRemote = true;
        endpoint.endpoint = 1;
        endpoint.outputClusters.push_back(0x0006);
        device.endpoints.push_back(endpoint);
        gateway->devices_[ieee] = device;
        gateway->nodeAddresses_[node] = ieee;
    }
    static bool restoredNetworkMaterial(const Gateway& gateway) {
        return gateway.networkPanId_ != 0 && gateway.networkPanId_ != 0xffff &&
               gateway.networkChannel_ == 11 && gateway.networkExtendedPan_.size() == 8 &&
               gateway.networkKey_.size() == 16 && gateway.preconfiguredKey_.size() == 16;
    }
    static void beginCoordinatorInitialization(Gateway* gateway) {
        gateway->online_ = true;
        gateway->ezspVersion_ = 8;
        gateway->networkInitRequested_ = false;
        gateway->coordinatorInitializationStep_ = 0;
        gateway->continueCoordinatorInitialization();
    }
    static void prepareApsSensor(Gateway* gateway) {
        Device device;
        EndpointDescriptor endpoint;
        device.ieeeAddress = "0x00124b0000001234";
        device.name = "APS sensor";
        device.networkAddress = 0x1234;
        device.hasTemperature = true;
        device.temperatureCelsius = 10.5;
        endpoint.endpoint = 1;
        endpoint.inputClusters.push_back(0x0402);
        device.endpoints.push_back(endpoint);
        gateway->devices_[device.ieeeAddress] = device;
        gateway->nodeAddresses_[device.networkAddress] = device.ieeeAddress;
    }
    static unsigned char ashReceiveSequence(const Gateway& gateway) { return gateway.ashReceiveSequence_; }
    static unsigned char ashSendSequence(const Gateway& gateway) { return gateway.ashSendSequence_; }
    static void setCoordinatorIeeeAddress(Gateway* gateway) {
        gateway->coordinatorIeeeAddress_.assign(8, 0);
        unsigned int i;
        for (i = 0; i < 8; ++i) gateway->coordinatorIeeeAddress_[i] = static_cast<unsigned char>(i + 1);
    }
    static void startDeviceInterview(Gateway* gateway, unsigned short node) {
        gateway->startInterview(node, false, true);
    }
    static void expireInterview(Gateway* gateway) { gateway->interviewDeadline_ = time(0) - 1; }
    static void expireSensorRefresh(Gateway* gateway, unsigned short node) {
        gateway->nextSensorRefreshAt_[node] = time(0) - 1;
    }
    static bool sensorRefreshScheduled(const Gateway& gateway, unsigned short node) {
        return gateway.nextSensorRefreshAt_.find(node) != gateway.nextSensorRefreshAt_.end();
    }
    static unsigned char coordinatorInitializationStep(const Gateway& gateway) { return gateway.coordinatorInitializationStep_; }
    static void expireCoordinatorInitialization(Gateway* gateway) {
        gateway->coordinatorInitializationDeadline_ = time(0) - 1;
    }
    static void expireResetDeadline(Gateway* gateway) { gateway->resetDeadline_ = time(0) - 1; }
    static bool sceneCommandErrors(Gateway* gateway) {
        std::string error;
        SceneGroup empty;
        Scene scene;
        SceneGroup configured;
        empty.id = 1; empty.name = "Empty";
        scene.id = 1; scene.name = "Scene"; empty.scenes.push_back(scene);
        configured.id = 2; configured.name = "Configured";
        configured.members.push_back("light"); configured.scenes.push_back(scene);
        gateway->sceneGroups_.push_back(empty);
        gateway->sceneGroups_.push_back(configured);
        if (gateway->activateScene(99, 1, &error) || error != "scene group not found") return false;
        if (gateway->turnOffSceneGroup(99, &error) || error != "scene group not found") return false;
        if (gateway->activateScene(1, 1, &error) || error != "scene group is empty") return false;
        if (gateway->turnOffSceneGroup(1, &error) || error != "scene group is empty") return false;
        if (gateway->activateScene(2, 9, &error) || error != "scene not found") return false;
        if (gateway->activateScene(2, 1, &error) || error != "Zigbee network is not ready") return false;
        if (!gateway->selectedScenes_.empty()) return false;
        gateway->online_ = true; gateway->networkFormed_ = true;
        gateway->coordinatorInitializationStep_ = 1;
        if (gateway->turnOffSceneGroup(2, &error) || error != "Zigbee network is not ready") return false;
        return gateway->selectedScenes_.empty();
    }
    static bool provisionSceneConfiguration(Gateway* gateway, const std::string& runtimeDirectory) {
        Device& device = gateway->devices_["native-light"];
        EndpointDescriptor endpoint;
        endpoint.endpoint = 1;
        endpoint.inputClusters.push_back(0x0004);
        endpoint.inputClusters.push_back(0x0005);
        endpoint.inputClusters.push_back(0x0006);
        endpoint.inputClusters.push_back(0x0008);
        device.ieeeAddress = "native-light"; device.networkAddress = 0x2345; device.hasLight = true;
        device.endpoints.push_back(endpoint);
        gateway->nodeAddresses_[device.networkAddress] = device.ieeeAddress;
        gateway->online_ = true; gateway->networkFormed_ = true; gateway->coordinatorInitializationStep_ = 0;
        const std::string configuration =
            "G\t42\tKitchen\nM\t42\tnative-light\nS\t42\t7\tDinner\nT\t42\t7\tnative-light\t1\t50\n";
        std::string error;
        if (!gateway->setSceneConfiguration(configuration, &error)) return false;
        std::vector<SceneGroup> groups;
        std::vector<RemoteSceneBinding> bindings;
        if (!loadSceneConfiguration(runtimeDirectory + "/scenes.conf", &groups, &bindings, &error) ||
            groups.size() != 1 || groups[0].id != 42 || groups[0].scenes.size() != 1) return false;
        std::string logs = gateway->logsJson();
        std::string status = gateway->statusJson();
        bool firstSent = logs.find("cluster=0x4") != std::string::npos &&
                      logs.find("payload=1101002a00074b69746368656e") != std::string::npos &&
                      status.find("\"scene_groups\":[{\"id\":42,\"name\":\"Kitchen\"") != std::string::npos;
        if (!firstSent || gateway->sceneProvisioning_[device.ieeeAddress].commands.size() != 2) return false;
        std::vector<unsigned char> response(19, 0);
        response[1] = 0x04; response[2] = 0x01; response[3] = 0x04;
        response[5] = 1; response[6] = 1;
        response[14] = 0x45; response[15] = 0x23; response[18] = 6;
        response.push_back(0x19); /* cluster response with default response disabled */
        response.push_back(gateway->sceneProvisioning_[device.ieeeAddress].commands[0].transaction);
        response.push_back(0x00); response.push_back(0x8a); response.push_back(42); response.push_back(0);
        gateway->handleIncomingMessage(response);
        if (gateway->sceneProvisioning_[device.ieeeAddress].index != 1) return false;
        gateway->pollSceneProvisioning();
        logs = gateway->logsJson();
        bool result = logs.find("cluster=0x5") != std::string::npos &&
                      logs.find("payload=1102002a000700000644696e6e6572060001010800017f") != std::string::npos;
        if (!result) fprintf(stderr, "scene provision logs: %s\nstatus: %s\n", logs.c_str(), status.c_str());
        return result;
    }
    static void sendSensitiveSecurityState(Gateway* gateway) {
        gateway->preconfiguredKey_.assign(16, 0x5a);
        gateway->networkKey_.assign(16, 0xa5);
        gateway->continueNetworkFormation(0x00b1);
    }
    static void logBoundedUnknownFrame(Gateway* gateway) {
        std::vector<unsigned char> message(512, 0xa5);
        message[0] = 0x11; message[1] = 0x2a; message[2] = 0xfe;
        gateway->logs_.clear(); gateway->logBytes_ = 0;
        gateway->addFrameLog("RX", 0x1234, 0x0104, 0x1234, 1, 1, 0, 0, 0, 0, 90, -40, message);
    }
    static void prepareHttpDevice(Gateway* gateway) {
        Device device;
        EndpointDescriptor power;
        EndpointDescriptor level;
        device.ieeeAddress = "light/test";
        device.networkAddress = 0x1234;
        device.hasLight = true;
        device.hasTemperature = true;
        device.hasContact = true;
        power.endpoint = 1;
        power.profileId = 0x0104;
        power.inputClusters.push_back(0x0006);
        power.inputClusters.push_back(0x0003);
        level.endpoint = 1;
        level.inputClusters.push_back(0x0008);
        device.endpoints.push_back(power);
        device.endpoints.push_back(level);
        gateway->devices_[device.ieeeAddress] = device;
        gateway->online_ = true;
        gateway->networkFormed_ = true;
        gateway->coordinatorInitializationStep_ = 0;
    }
    static void prepareRunnerCommandDevice(Gateway* gateway) {
        Device device;
        EndpointDescriptor endpoint;
        device.ieeeAddress = "runner-light";
        device.name = "Runner light";
        device.networkAddress = 0x2345;
        device.hasLight = true;
        device.hasTemperature = true;
        device.sensorRefreshIntervalMinutes = 0;
        endpoint.endpoint = 1;
        endpoint.inputClusters.push_back(0x0004);
        endpoint.inputClusters.push_back(0x0005);
        endpoint.inputClusters.push_back(0x0006);
        endpoint.inputClusters.push_back(0x0008);
        endpoint.inputClusters.push_back(0x0402);
        device.endpoints.push_back(endpoint);
        gateway->devices_[device.ieeeAddress] = device;
        gateway->nodeAddresses_[device.networkAddress] = device.ieeeAddress;
        gateway->online_ = true;
        gateway->networkFormed_ = true;
        gateway->networkInitRequested_ = true;
        gateway->coordinatorInitializationStep_ = 0;
        gateway->ezspVersion_ = 4;
    }
};

#endif
