#ifndef MICROHAD_ZIGBEE_SHELLY_INTEGRATION_H
#define MICROHAD_ZIGBEE_SHELLY_INTEGRATION_H

#include <pthread.h>
#include <string>
#include <vector>

class GatewayRunner;

struct ShellyDevice {
    std::string id;
    std::string name;
    std::string host;
    std::string callbackHost;
    std::string model; /* uni, plug, plugs */
    std::string username;
    std::string password;
    std::string callbackToken;
    int inputIndex;
    int temperatureIndex;
    int relayIndex;
    unsigned int pollSeconds;
    bool invertContact;
    long nextPoll;
    ShellyDevice();
};

class ShellyIntegration {
public:
    ShellyIntegration(const std::string& runtimeDirectory, GatewayRunner* gateway = 0);
    ~ShellyIntegration();
    bool start(std::string* error);
    void stop();
    std::string json(bool includeCallbackUrls = false) const;
    bool saveDevice(const std::string& id, const std::string& name, const std::string& host, const std::string& callbackHost,
                    const std::string& model, const std::string& username, const std::string& password,
                    int inputIndex, int temperatureIndex, int relayIndex, unsigned int pollSeconds,
                    bool invertContact, bool callbackMode, bool clearAuth, std::string* resultId, std::string* error);
    bool removeDevice(const std::string& id, std::string* error);
    bool setActor(const std::string& id, bool on, std::string* error);
    bool refresh(const std::string& id, std::string* error);
    bool callback(const std::string& id, const std::string& event, const std::string& token, bool* overloaded = 0);
    static bool parsePowerStatus(const std::string& body, double* watts);

private:
    struct ActorCommand { std::string id; bool on; };
    struct CallbackEvent { std::string id; int metric; double value; };
    std::string runtimeDirectory_;
    GatewayRunner* gateway_;
    mutable pthread_mutex_t mutex_;
    pthread_cond_t ready_;
    pthread_t thread_;
    bool running_;
    bool started_;
    std::vector<ShellyDevice> devices_;
    std::vector<ActorCommand> commands_;
    std::vector<CallbackEvent> callbacks_;
    static void* threadMain(void* opaque);
    void run();
    bool load(std::string* error);
    bool persist(std::string* error) const;
    void pollDevices();
    void executeCommands();
    void executeCallbacks();
};

#endif
