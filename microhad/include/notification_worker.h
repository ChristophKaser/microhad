#ifndef MICROHAD_ZIGBEE_NOTIFICATION_WORKER_H
#define MICROHAD_ZIGBEE_NOTIFICATION_WORKER_H

#include <pthread.h>
#include <string>
#include <vector>

class NotificationWorker {
public:
    struct RouteClient { std::string id, presenceSensorId; bool enabled; };
    class DeliveryHook {
    public:
        virtual ~DeliveryHook() {}
        virtual void deliver(const std::string& title, const std::string& body,
                             const std::string& androidChannelId) = 0;
    };
    NotificationWorker(const std::string& runtimeDirectory, const std::string& firebaseServiceAccount,
                       const std::string& firebaseTopic, const std::string& caBundle,
                       DeliveryHook* deliveryHook = 0);
    ~NotificationWorker();
    bool start(std::string* error);
    void stop();
    bool enqueue(const std::string& title, const std::string& body,
                 const std::vector<std::string>& presentSensorIds, std::string* error,
                 const std::string& androidChannelId = "");
    bool enqueue(const std::string& title, const std::string& body, std::string* error,
                 const std::string& androidChannelId = "");
    bool enabled() const;
    bool setEnabled(bool enabled, std::string* error);
    bool registerDevice(const std::string& id, const std::string& model, const std::string& token, std::string* error);
    bool updateDevice(const std::string& id, const std::string& label, bool enabled,
                      const std::string& presenceSensorId, std::string* error);
    bool updateDevice(const std::string& id, const std::string& label, bool enabled, std::string* error);
    bool removeDevice(const std::string& id, std::string* error);
    std::string devicesJson() const;
    static std::vector<std::string> selectRecipients(const std::vector<RouteClient>& clients,
                                                     const std::vector<std::string>& presentSensorIds);
private:
    struct Job { std::string title; std::string body; std::string androidChannelId; std::vector<std::string> presentSensorIds; };
    struct Device { std::string id, model, token, label, presenceSensorId; bool enabled; };
    std::string runtimeDirectory_, firebaseServiceAccount_, firebaseTopic_, caBundle_;
    DeliveryHook* deliveryHook_;
    pthread_t thread_;
    mutable pthread_mutex_t mutex_;
    pthread_cond_t ready_;
    std::vector<Job> jobs_;
    std::vector<Device> devices_;
    bool running_, started_, enabled_;
    static void* threadMain(void* opaque);
    void run();
    void deliver(const Job& job);
    void loadEnabled();
    bool saveEnabled(std::string* error) const;
    void loadDevices();
    bool saveDevices(std::string* error) const;
};
#endif
