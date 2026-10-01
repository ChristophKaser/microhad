#ifndef MICROHAD_ZIGBEE_HTTP_SERVER_H
#define MICROHAD_ZIGBEE_HTTP_SERVER_H

#include <string>
#include <vector>
#include <map>
#include <pthread.h>
#include <stdint.h>

class Gateway;
class GatewayRunner;
class WebPushStore;
class ExternalHttpDeviceWorker;
class NotificationWorker;

class HttpServer {
public:
    HttpServer(int port, GatewayRunner* gateway, const std::string& webuiDirectory,
               const std::string& certificatePath, const std::string& privateKeyPath,
               ExternalHttpDeviceWorker* shelly = 0, NotificationWorker* notifications = 0);
    ~HttpServer();
    bool start(std::string* error);
    void poll();
    int boundPort() const;

private:
    int port_;
    int listenFd_;
    GatewayRunner* gateway_;
    unsigned long processId_;
    ExternalHttpDeviceWorker* shelly_;
    NotificationWorker* notifications_;
    WebPushStore* webPush_;
    std::string webuiDirectory_;
    std::string certificatePath_;
    std::string privateKeyPath_;
    void* tlsServer_;
    pthread_t acceptThread_;
    pthread_t workerThreads_[2];
    pthread_mutex_t queueMutex_;
    pthread_cond_t queueReady_;
    std::vector<int> clientQueue_;
    bool running_;
    pthread_mutex_t statsMutex_;
    uint64_t previousCpuTotal_;
    uint64_t previousCpuIdle_;
    bool hasPreviousCpu_;
    struct ThreadCpuSample { uint64_t startTicks; uint64_t cpuTicks; };
    std::map<uint64_t, ThreadCpuSample> previousThreadSamples_;
    double previousThreadSampleUptime_;
    void handleClient(int clientFd);
    void handleRequest(int clientFd, const std::string& request);
    std::string controllerStatsJson();
    static void* acceptThreadMain(void* opaque);
    static void* workerThreadMain(void* opaque);
    void acceptLoop();
    void workerLoop();
};

#endif
