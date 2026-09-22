#ifndef MICROHAD_ZIGBEE_GATEWAY_RUNNER_H
#define MICROHAD_ZIGBEE_GATEWAY_RUNNER_H

#include <pthread.h>
#include <string>

class Gateway;

class GatewayRunner {
public:
    GatewayRunner(Gateway* gateway);
    ~GatewayRunner();
    bool start(std::string* error);
    void stop();
    std::string statusJson() const;
    std::string logsJson() const;
    std::string runtimeDirectory() const;
    bool setJoining(unsigned int seconds, std::string* error);
    bool createFreshNetwork(std::string* error);
    bool setDeviceSettings(const std::string& ieeeAddress, const std::string& name,
                           bool hasLow, double low, bool hasHigh, double high,
                           bool hasContactTimings, unsigned long initialAlertMinutes,
                           unsigned long reminderAlertMinutes, bool hasSensorRefreshInterval,
                           unsigned long sensorRefreshIntervalMinutes, std::string* error);
    bool refreshDevice(const std::string& ieeeAddress, std::string* error);
    bool sendTestNotification(std::string* error);
    bool setNotificationsEnabled(bool enabled, std::string* error);

private:
    enum CommandType { Join, CreateNetwork, DeviceSettings, DeviceRefresh, TestNotification, NotificationSetting };
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
    pthread_cond_t commandReady_;
    pthread_cond_t startupReady_;
    Command* commandHead_;
    Command* commandTail_;
    bool started_;
    bool startupComplete_;
    bool startupSucceeded_;
    bool stopping_;
    std::string startupError_;
    std::string statusJson_;
    std::string logsJson_;
    std::string runtimeDirectory_;
    static void* runThread(void* opaque);
    void run();
    bool invoke(Command* command, std::string* error);
    void execute(Command* command);
    void updateSnapshots();
};

#endif
