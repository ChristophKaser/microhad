#ifndef MICROHAD_ZIGBEE_GATEWAY_INTEGRATION_PEER_H
#define MICROHAD_ZIGBEE_GATEWAY_INTEGRATION_PEER_H

#include "gateway.h"

#include <cassert>
#include <ctime>
#include <fcntl.h>

class GatewayIntegrationTestPeer {
public:
    static void attachSerial(Gateway* gateway, int fd) {
        assert(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
        gateway->serialFd_ = fd;
    }
    static bool online(const Gateway& gateway) { return gateway.online_; }
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
        power.inputClusters.push_back(0x0006);
        level.endpoint = 1;
        level.inputClusters.push_back(0x0008);
        device.endpoints.push_back(power);
        device.endpoints.push_back(level);
        gateway->devices_[device.ieeeAddress] = device;
        gateway->online_ = true;
        gateway->networkFormed_ = true;
        gateway->coordinatorInitializationStep_ = 0;
    }
};

#endif
