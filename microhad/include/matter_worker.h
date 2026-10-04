#ifndef MICROHAD_MATTER_WORKER_H
#define MICROHAD_MATTER_WORKER_H

#include "matter_client.h"
#include <pthread.h>
#include <map>

class GatewayRunner;

// One IPC owner; HTTP reads only cached, validated snapshots.
class MatterWorker {
public:
    explicit MatterWorker(GatewayRunner* gateway, const std::string& socketPath = "/tmp/microhad-matter.sock");
    ~MatterWorker();
    bool start(std::string* error);
    void stop();
    bool status(bool thread, std::string* json, std::string* error);
    bool submit(unsigned char opcode, const std::string& body, uint32_t* id, std::string* error);
    bool job(uint32_t id, std::string* json);
    bool takeEvent(std::string* json);

private:
    GatewayRunner* gateway_;
    MatterChannel client_;
    struct Job {
        unsigned char opcode;
        std::string body;
        std::string state;
        std::string response;
        Job() : opcode(0) {}
    };
    std::map<uint32_t, Job> jobs_;
    std::deque<uint32_t> pending_;
    std::deque<std::string> events_;
    uint32_t nextJob_;
    void unavailable();
    bool collectEvents();
    void executeJob();
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
