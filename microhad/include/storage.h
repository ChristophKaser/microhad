#ifndef MICROHAD_ZIGBEE_STORAGE_H
#define MICROHAD_ZIGBEE_STORAGE_H

#include <string>
#include <vector>

struct Device;
struct SceneGroup;
struct RemoteSceneBinding;
struct NativeRemoteBinding;

struct NetworkState {
    bool formed;
    unsigned short panId;
    unsigned char channel;
    std::vector<unsigned char> extendedPan;
    std::vector<unsigned char> networkKey;
    std::vector<unsigned char> preconfiguredKey;
    NetworkState();
};

bool ensureDirectory(const std::string& directory, std::string* error);
bool loadDevices(const std::string& filename, std::vector<Device>* devices, std::string* error);
bool saveDevices(const std::string& filename, const std::vector<Device>& devices, std::string* error);
bool loadNetworkState(const std::string& filename, NetworkState* network, std::string* error);
bool saveNetworkState(const std::string& filename, const NetworkState& network, std::string* error);
bool loadSceneConfiguration(const std::string& filename, std::vector<SceneGroup>* groups,
                            std::vector<RemoteSceneBinding>* bindings, std::string* error);
bool loadSceneConfiguration(const std::string& filename, std::vector<SceneGroup>* groups,
                            std::vector<RemoteSceneBinding>* bindings,
                            std::vector<NativeRemoteBinding>* nativeBindings, std::string* error);
bool saveSceneConfiguration(const std::string& filename, const std::vector<SceneGroup>& groups,
                            const std::vector<RemoteSceneBinding>& bindings, std::string* error);
bool saveSceneConfiguration(const std::string& filename, const std::vector<SceneGroup>& groups,
                            const std::vector<RemoteSceneBinding>& bindings,
                            const std::vector<NativeRemoteBinding>& nativeBindings, std::string* error);
#endif
