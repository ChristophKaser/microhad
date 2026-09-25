#ifndef MICROHAD_ZIGBEE_GATEWAY_RUNNER_H
#define MICROHAD_ZIGBEE_GATEWAY_RUNNER_H

#include <pthread.h>
#include <string>

class Gateway;
class GatewayView;

class GatewayRunner {
    struct ViewData;
public:
    class ViewLease {
    public:
        ~ViewLease();
        std::string statusJson() const;
        std::string logsJson() const;
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
    bool setJoining(unsigned int seconds, std::string* error);
    bool createFreshNetwork(std::string* error);
    bool setDeviceSettings(const std::string& ieeeAddress, const std::string& name,
                           bool hasLow, double low, bool hasHigh, double high,
                           bool hasContactTimings, unsigned long initialAlertMinutes,
                           unsigned long reminderAlertMinutes, bool hasSensorRefreshInterval,
                           unsigned long sensorRefreshIntervalMinutes, std::string* error);
    bool refreshDevice(const std::string& ieeeAddress, std::string* error);
    bool identifyDevice(const std::string& ieeeAddress, std::string* error);
    bool setLightState(const std::string& ieeeAddress, const std::string& state, int brightnessPercent,
                       std::string* error);
    bool setSceneConfiguration(const std::string& configuration, std::string* error);
    bool activateScene(unsigned short groupId, unsigned char sceneId, std::string* error);
    bool turnOffSceneGroup(unsigned short groupId, std::string* error);
    bool sendTestNotification(std::string* error);
    bool setNotificationsEnabled(bool enabled, std::string* error);

private:
    enum CommandType { Join, CreateNetwork, DeviceSettings, DeviceRefresh, DeviceIdentify, LightState, SceneConfiguration, SceneActivate, SceneOff, TestNotification, NotificationSetting };
    struct Command {
        CommandType type;
        unsigned int seconds;
        std::string address;
        std::string name;
        bool hasLow;
        double low;
        bool hasHigh;
        double high;
        bool hasContactTimings;
        unsigned long initialAlertMinutes;
        unsigned long reminderAlertMinutes;
        bool hasSensorRefreshInterval;
        unsigned long sensorRefreshIntervalMinutes;
        int brightnessPercent;
        unsigned short groupId;
        unsigned char sceneId;
        std::string configuration;
        bool result;
        bool complete;
        std::string error;
        pthread_mutex_t mutex;
        pthread_cond_t ready;
        Command* next;
    };

    Gateway* gateway_;
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
    std::string startupError_;
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
    void waitForWork();
};

#endif
