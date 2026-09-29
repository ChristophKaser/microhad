#ifndef MICROHAD_ZIGBEE_EXTERNAL_HTTP_DEVICE_H
#define MICROHAD_ZIGBEE_EXTERNAL_HTTP_DEVICE_H

#include <pthread.h>
#include <string>
#include <vector>
#include "device_shelly.h"
#include "device_fritz_box.h"

class GatewayRunner;

bool externalHttpRequest(const std::string& host, const std::string& username, const std::string& password,
                        const std::string& method, const std::string& path, const std::string& headers,
                        const std::string& requestBody, std::string* responseBody);

class ExternalHttpDeviceWorker {
public:
    ExternalHttpDeviceWorker(const std::string& runtimeDirectory, GatewayRunner* gateway = 0);
    ~ExternalHttpDeviceWorker();
    bool start(std::string* error);
    void stop();
    std::string json(bool includeCallbackUrls = false) const;
    bool saveDevice(const std::string& id, const std::string& name, const std::string& host, const std::string& callbackHost,
                    const std::string& model, const std::string& username, const std::string& password,
                    const std::vector<int>& inputIndices, const std::vector<int>& temperatureIndices,
                    const std::vector<int>& relayIndices, unsigned int pollSeconds,
                    bool invertContact, bool callbackMode, bool clearAuth, std::string* resultId, std::string* error);
    bool saveDevice(const std::string& id, const std::string& name, const std::string& host, const std::string& callbackHost,
                    const std::string& model, const std::string& username, const std::string& password,
                    int inputIndex, int temperatureIndex, int relayIndex, unsigned int pollSeconds,
                    bool invertContact, bool callbackMode, bool clearAuth, std::string* resultId, std::string* error);
    bool removeDevice(const std::string& id, std::string* error);
    bool setActor(const std::string& id, int relayIndex, bool on, std::string* error);
    bool setActor(const std::string& id, bool on, std::string* error) { return setActor(id, 0, on, error); }
    bool refresh(const std::string& id, std::string* error);
    bool callback(const std::string& id, const std::string& event, const std::string& token, bool* overloaded = 0);
    static bool parsePowerStatus(const std::string& body, double* watts);
    std::string fritzJson() const;
    bool discoverFritzClients(const std::string& host, const std::string& username, const std::string& password,
                              std::vector<FritzBoxClient>* clients, std::string* error);
    bool saveFritzBox(const std::string& id, const std::string& name, const std::string& host,
                      const std::string& username, const std::string& password,
                      const std::vector<FritzBoxClient>& clients, std::string* resultId, std::string* error);
    bool removeFritzBox(const std::string& id, std::string* error);
    static bool parseFritzHostList(const std::string& xml, std::vector<FritzBoxClient>* clients);

private:
    struct ActorCommand { std::string id; int relayIndex; bool on; };
    struct CallbackEvent { std::string id; int metric; int channelIndex; double value; };
    struct DiscoveryRequest { std::string host, username, password, error; std::vector<FritzBoxClient> clients; pthread_cond_t ready; bool done, result; DiscoveryRequest() : done(false), result(false) { pthread_cond_init(&ready, 0); } ~DiscoveryRequest() { pthread_cond_destroy(&ready); } };
    std::string runtimeDirectory_;
    GatewayRunner* gateway_;
    mutable pthread_mutex_t mutex_;
    pthread_cond_t ready_;
    pthread_t thread_;
    bool running_;
    bool started_;
    std::vector<ShellyDevice> devices_;
    std::vector<FritzBoxDevice> fritzBoxes_;
    std::vector<ActorCommand> commands_;
    std::vector<CallbackEvent> callbacks_;
    std::vector<DiscoveryRequest*> discoveries_;
    static void* threadMain(void* opaque);
    void run();
    bool load(std::string* error);
    bool persist(std::string* error) const;
    void pollDevices();
    void pollFritzBoxes();
    void executeDiscoveries();
    bool discoverFritzClientsNetwork(const std::string& host, const std::string& username, const std::string& password,
                                    std::vector<FritzBoxClient>* clients, std::string* error);
    bool loadFritzBoxes(std::string* error);
    bool persistFritzBoxes(std::string* error) const;
    void executeCommands();
    void executeCallbacks();
};

#endif
