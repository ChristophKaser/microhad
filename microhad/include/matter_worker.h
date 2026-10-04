#ifndef MICROHAD_MATTER_WORKER_H
#define MICROHAD_MATTER_WORKER_H

#include "matter_client.h"
#include <pthread.h>

class GatewayRunner;

// One IPC owner; HTTP reads only cached, validated snapshots.
class MatterWorker {
public:
    explicit MatterWorker(GatewayRunner* gateway, const std::string& socketPath = "/tmp/microhad-matter.sock");
    ~MatterWorker();
    bool start(std::string* error);
    void stop();
    bool status(bool thread, std::string* json, std::string* error);
private:
    GatewayRunner* gateway_;
    MatterClient client_;
    pthread_mutex_t mutex_;
    pthread_t thread_;
    bool running_;
    bool started_;
    std::string matterStatus_;
    std::string threadStatus_;
    int64_t refreshed_;
    bool running();
    void run();
    static void* threadMain(void* opaque);
    MatterWorker(const MatterWorker&);
    MatterWorker& operator=(const MatterWorker&);
};
#endif
