#ifndef MICROHAD_ZIGBEE_GATEWAY_RUNNER_H
#define MICROHAD_ZIGBEE_GATEWAY_RUNNER_H

#include <pthread.h>
#include <map>
#include <string>
#include <vector>
#include "gateway.h"

class Gateway;
class GatewayView;
class SensorHistory;

class GatewayRunner {
    struct ViewData;
public:
    class ViewLease {
    public:
        ~ViewLease();
        std::string statusJson() const;
        long serialOpenedAt() const;
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
    std::string runtimeDirectory() const;
    std::string historyPath() const;
    bool readHistoryJson(const std::string& address, unsigned long before,
                         std::string* json, std::string* error) const;
    bool registerShelly(const std::string& id, const std::string& name, const std::string& oldName,
                        const std::string& model, const std::vector<int>& inputIndices,
                        const std::vector<int>& temperatureIndices, const std::vector<int>& relayIndices,
                        std::string* error);
    bool removeShelly(const std::string& id, std::string* error);
    bool reconcileShelly(const std::vector<std::string>& ids, std::string* error);
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
    bool refreshDevice(const std::string& ieeeAddress, std::string* error);
    bool identifyDevice(const std::string& ieeeAddress, std::string* error);
    bool queryDeviceConfiguration(const std::string& ieeeAddress, std::string* error);
    bool setLightState(const std::string& ieeeAddress, const std::string& state, int brightnessPercent,
                       std::string* error);
    bool setSceneConfiguration(const std::string& configuration, std::string* error);
    bool activateScene(unsigned short groupId, unsigned char sceneId, std::string* error);
    bool turnOffSceneGroup(unsigned short groupId, std::string* error);
    bool sendTestNotification(std::string* error);
    bool setNotificationsEnabled(bool enabled, std::string* error);
    bool setEventLoopDebug(bool enabled, std::string* error);

private:
    enum CommandType { NetworkBackup, Join, CreateNetwork, DeviceSettings, DeviceValueAlerts, DeviceRefresh, DeviceIdentify, DeviceConfiguration, LightState, SceneConfiguration, SceneActivate, SceneOff, TestNotification, NotificationSetting, EventLoopDebug, ShellyRegister, ShellyRemove, ShellyReconcile, ShellyEvent, FritzEvent, FritzReconcile, ActorState };
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
        bool result;
        bool enabled;
        bool complete;
        std::string error;
        pthread_mutex_t mutex;
        pthread_cond_t ready;
        Command* next;
    };

    Gateway* gateway_;
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
        ViewData(GatewayView* value) : view(value), references(0), retired(false) {}
    };
    ViewData* currentView_;
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
