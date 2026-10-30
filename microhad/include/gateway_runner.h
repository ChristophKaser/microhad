#ifndef MICROHAD_ZIGBEE_GATEWAY_RUNNER_H
#define MICROHAD_ZIGBEE_GATEWAY_RUNNER_H

#include <pthread.h>
#include <stdint.h>
#include <map>
#include <string>
#include <vector>
#include "gateway.h"
#include "automation_types.h"
#include "sensor_history.h"

class Gateway;
class GatewayView;
class SensorHistory;
class AutomationWorker;
class MatterWorker;

class GatewayRunner {
#ifdef MICROHAD_TESTING
    friend class AutomationRunnerTestPeer;
#endif
    struct ViewData;
public:
    class ViewLease {
    public:
        ~ViewLease();
        std::string statusJson() const;
        std::string settingsJson() const;
        std::string matterCountryCode() const;
        std::string matterRegulatoryLocation() const;
#ifdef MICROHAD_TESTING
        std::string logsJson() const;
#endif
        long serialOpenedAt() const;
        bool coordinatorOnline() const;
        unsigned char coordinatorChannel() const;
        std::vector<Device> devices() const;
        std::vector<SceneGroup> sceneGroups() const;
    private:
        friend class GatewayRunner;
        ViewData* data_;
        GatewayRunner* owner_;
        ViewLease(GatewayRunner* owner, ViewData* data);
        ViewLease(const ViewLease&);
        ViewLease& operator=(const ViewLease&);
    };

    GatewayRunner(Gateway* gateway);
    ~GatewayRunner();
    bool start(std::string* error);
    void stop();
    ViewLease* acquireView();
    void coordinatorObservation(unsigned char* channel, bool* online);
    std::string runtimeDirectory() const;
    std::string historyPath() const;
#ifdef MICROHAD_TESTING
    std::string logsJson() const;
#endif
    std::vector<std::string> historyDeviceIds(const std::string& address) const;
    SensorHistory* historyStore() const { return historyStore_; }
    bool readHistoryJson(const std::string& address, uint64_t before,
                         std::string* json, std::string* error, uint32_t since = 0) const;
    bool registerShelly(const std::string& id, const std::string& name, const std::string& oldName,
                        const std::string& model, const std::vector<int>& inputIndices,
                        const std::vector<int>& temperatureIndices, const std::vector<int>& relayIndices,
                        std::string* error);
    bool removeShelly(const std::string& id, std::string* error);
    bool reconcileShelly(const std::vector<std::string>& ids, std::string* error);
    bool reconcileMatterDevices(const std::vector<MatterEndpoint>& endpoints, std::string* error);
    bool applyMatterReading(const MatterReading& reading, std::string* error);
    bool applyMatterButtonEvent(const MatterButtonEvent& event, std::string* error);
    bool reconcileFritzClients(const std::vector<std::string>& ids, std::string* error);
    bool applyShellyEvent(const std::string& id, int metric, int channelIndex, double value, std::string* error);
    bool applyFritzEvent(const std::string& id, bool connected, bool hasSignal, int signalPercent, std::string* error);
    bool setActorState(const std::string& address, bool on, std::string* error);
    bool setJoining(unsigned int seconds, std::string* error);
    bool createFreshNetwork(std::string* error);
    bool exportNetworkBackup(const std::string& id, std::string* error);
    bool setDeviceSettings(const std::string& ieeeAddress, const std::string& name,
                           bool hasLow, double low, bool hasHigh, double high,
                           bool hasContactTimings, unsigned long initialAlertMinutes,
                           unsigned long reminderAlertMinutes, bool hasSensorRefreshInterval,
                           unsigned long sensorRefreshIntervalMinutes, std::string* error,
                           bool hasContactTriggerSettings = false, bool contactTriggerMode = false,
                           unsigned long contactTriggerCooldownSeconds = 60);
    bool setDeviceValueAlerts(const std::string& address, const std::map<std::string, DeviceValueAlert>& alerts,
                              std::string* error);
    bool deleteDevice(const std::string& address, bool local, std::string* error);
    bool deviceDeletionStatus(const std::string& address, std::string* json, std::string* error);
    bool matterDeletionTarget(const std::string& address, std::string* node, std::string* error);
    bool removeMatterNode(const std::string& node, std::string* error);
    bool reconcileMatterNodes(const std::vector<std::string>& nodes, std::string* error);
    bool refreshDevice(const std::string& ieeeAddress, std::string* error);
    bool reconfigureDevice(const std::string& ieeeAddress, std::string* error);
    bool startOta(const std::string& address, const std::string& image, std::string* error);
    bool identifyDevice(const std::string& ieeeAddress, std::string* error);
    bool queryDeviceConfiguration(const std::string& ieeeAddress, std::string* error);
    bool setThermostat(const std::string& address, const std::string& field,
                       const std::string& value, std::string* error, MatterProto::MessageBody* matterRequest = 0);
    bool thermostatResult(const std::string& node, unsigned short endpoint, const std::string& field, const std::string& error);
    bool setLightState(const std::string& ieeeAddress, const std::string& state, int brightnessPercent,
                       std::string* error);
    bool setSceneConfiguration(const std::string& configuration, std::string* error);
    bool activateScene(unsigned short groupId, unsigned char sceneId, std::string* error);
    bool turnOffSceneGroup(unsigned short groupId, std::string* error);
    bool sendTestNotification(std::string* error);
    bool setNotificationsEnabled(bool enabled, std::string* error);
    bool setEventLoopDebug(bool enabled, std::string* error);
    bool setBroadcastQuorum(unsigned int value, std::string* error);
    bool setMatterCommissioningRegion(const std::string& countryCode, const std::string& location,
                                     std::string* error);
    Automation::CatalogPtr acquireAutomationCatalog();
    void setAutomationWorker(AutomationWorker* worker);
    void setAutomationMatterWorker(MatterWorker* worker);
    void automationGeneration(const std::string& script, unsigned long generation);
    bool submitAutomation(const Automation::Action& action, std::string* error);

private:
    enum CommandType { DeviceDelete, DeviceDeleteStatus, MatterDeleteTarget, MatterNodeRemove, MatterNodesReconcile, GatewaySettings, MatterRegionSettings, NetworkBackup, Join, CreateNetwork, DeviceSettings, DeviceValueAlerts, DeviceRefresh, DeviceReconfigure, DeviceIdentify, OtaStart, DeviceConfiguration, LightState, ThermostatSetting, ThermostatResult, SceneConfiguration, SceneActivate, SceneOff, TestNotification, NotificationSetting, EventLoopDebug, ShellyRegister, ShellyRemove, ShellyReconcile, MatterReconcile, MatterValue, MatterButton, ShellyEvent, FritzEvent, FritzReconcile, ActorState };
    struct Command {
        CommandType type;
        unsigned int seconds;
        std::string address;
        std::string name;
        std::string oldName;
        std::string model;
        std::vector<std::string> ids;
        std::vector<int> inputIndices;
        std::vector<int> temperatureIndices;
        std::vector<int> relayIndices;
        std::vector<MatterEndpoint> matterEndpoints;
        MatterReading matterReading;
        MatterButtonEvent matterButton;
        int metric;
        double value;
        bool on;
        int temperatureIndex;
        int channelIndex;
        bool hasLow;
        double low;
        bool hasHigh;
        double high;
        bool hasContactTimings;
        unsigned long initialAlertMinutes;
        unsigned long reminderAlertMinutes;
        bool hasSensorRefreshInterval;
        unsigned long sensorRefreshIntervalMinutes;
        bool hasContactTriggerSettings;
        bool contactTriggerMode;
        unsigned long contactTriggerCooldownSeconds;
        std::map<std::string, DeviceValueAlert> valueAlerts;
        int brightnessPercent;
        unsigned short groupId;
        unsigned char sceneId;
        std::string configuration;
        MatterProto::MessageBody matterRequest;
        bool result;
        bool enabled;
        bool complete;
        std::string error;
        pthread_mutex_t mutex;
        pthread_cond_t ready;
        Command* next;
    };

    Gateway* gateway_;
    AutomationWorker* automation_;
    MatterWorker* automationMatter_;
    std::deque<Automation::Action> automationActions_;
    std::map<std::string, unsigned long> automationGenerations_;
    std::map<std::string, Automation::SnapshotPtr> automationSnapshots_;
    Automation::CatalogPtr automationCatalog_;
    uint64_t automationRevision_;
    uint64_t automationChain_;
    uint64_t activeAutomationChain_;
    unsigned int activeAutomationDepth_;
    void publishAutomation(bool force = false);
    void executeAutomation(const Automation::Action& action);
    SensorHistory* historyStore_;
    pthread_t thread_;
    mutable pthread_mutex_t mutex_;
    pthread_cond_t startupReady_;
    int commandWakeRead_;
    int commandWakeWrite_;
    Command* commandHead_;
    Command* commandTail_;
    bool started_;
    bool startupComplete_;
    bool startupSucceeded_;
    bool stopping_;
    bool eventLoopDebug_;
    std::string startupError_;
    struct LoopDebugStats {
        unsigned long iterations;
        unsigned long commandSkips;
        unsigned long immediateSkips;
        unsigned long selectCalls;
        unsigned long selectReady;
        unsigned long selectTimeouts;
        unsigned long selectInterrupted;
        unsigned long selectErrors;
        unsigned long serialReadReady;
        unsigned long serialWriteReady;
        unsigned long wakeReady;
        int lastSelectResult;
        int lastSelectErrno;
        int lastTimeoutSeconds;
        long lastDeadline;
        std::string lastDeadlineSource;
        LoopDebugStats();
    };
    struct ViewData {
        GatewayView* view;
        unsigned long references;
        bool retired;
        mutable pthread_mutex_t statusMutex;
        mutable std::string cachedStatus;
        mutable long statusSecond;
        mutable bool statusCached;
        ViewData(GatewayView* value) : view(value), references(0), retired(false), statusSecond(0), statusCached(false) { pthread_mutex_init(&statusMutex,0); }
        ~ViewData() { pthread_mutex_destroy(&statusMutex); }
        std::string statusJson() const;
    private:
        ViewData(const ViewData&);
        ViewData& operator=(const ViewData&);
    };
    ViewData* currentView_;
    int64_t lastCoordinatorProgress_;
    std::string runtimeDirectory_;
    std::string historyPath_;
    static void* runThread(void* opaque);
    void run();
    bool invoke(Command* command, std::string* error);
    void execute(Command* command);
    void publishView(bool force);
    void releaseView(ViewData* data);
    void wakeCoordinator();
    void waitForWork(LoopDebugStats* stats, bool debugEnabled);
};

#endif
