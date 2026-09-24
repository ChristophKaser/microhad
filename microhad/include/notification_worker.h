#ifndef MICROHAD_ZIGBEE_NOTIFICATION_WORKER_H
#define MICROHAD_ZIGBEE_NOTIFICATION_WORKER_H

#include <pthread.h>
#include <string>
#include <vector>

class NotificationWorker {
public:
    class DeliveryHook {
    public:
        virtual ~DeliveryHook() {}
        virtual void deliver(const std::string& title, const std::string& body) = 0;
    };
    NotificationWorker(const std::string& runtimeDirectory, const std::string& firebaseServiceAccount,
                       const std::string& firebaseTopic, const std::string& caBundle,
                       DeliveryHook* deliveryHook = 0);
    ~NotificationWorker();
    bool start(std::string* error);
    void stop();
    bool enqueue(const std::string& title, const std::string& body, std::string* error);
    bool enabled() const;
    bool setEnabled(bool enabled, std::string* error);
private:
    struct Job { std::string title; std::string body; };
    std::string runtimeDirectory_, firebaseServiceAccount_, firebaseTopic_, caBundle_;
    DeliveryHook* deliveryHook_;
    pthread_t thread_;
    mutable pthread_mutex_t mutex_;
    pthread_cond_t ready_;
    std::vector<Job> jobs_;
    bool running_, started_, enabled_;
    static void* threadMain(void* opaque);
    void run();
    void deliver(const Job& job);
    void loadEnabled();
    bool saveEnabled(std::string* error) const;
};
#endif
