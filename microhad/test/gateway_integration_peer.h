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
        bool result = logs.find("cluster=0x4") != std::string::npos && logs.find("cluster=0x5") != std::string::npos &&
                      logs.find("payload=1101002a00074b69746368656e") != std::string::npos &&
                      logs.find("payload=1102002a000700000644696e6e6572060001010800017f") != std::string::npos &&
                      status.find("\"scene_groups\":[{\"id\":42,\"name\":\"Kitchen\"") != std::string::npos;
        if (!result) fprintf(stderr, "scene provision logs: %s\nstatus: %s\n", logs.c_str(), status.c_str());
        return result;
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
