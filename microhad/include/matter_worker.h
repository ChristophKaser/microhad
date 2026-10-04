#ifndef MICROHAD_MATTER_WORKER_H
#define MICROHAD_MATTER_WORKER_H

#include "matter_client.h"
#include "matter_websocket.h"
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
    bool adoptRelay(int fd, const std::string& handshake, std::string* error);

private:
    GatewayRunner* gateway_;
    int phonePendingFd_;
    std::string phoneHandshake_;
    bool phoneActive_;
    int phoneFd_;
    bool phoneReady_;
    MatterWebSocket::Decoder phoneDecoder_;
    std::deque<std::string> phoneOutput_;
    unsigned long phoneOffset_;
    int64_t phoneReceived_;
    int64_t phoneWriteSince_;
    uint32_t phoneLastId_;
    uint32_t phoneGeneration_;
    std::string phoneSession_;
    bool phoneQueue(unsigned char opcode, const std::string& payload);
    void phonePump();
    void dropPhone(bool cancel, unsigned int code);

    MatterChannel client_;
    struct Job {
        unsigned char opcode;
        std::string body;
        std::string state;
        std::string response;
        std::string error;
        std::string serviceJob;
        int64_t pollAt;
        Job() : opcode(0), pollAt(0) {}
    };
    std::map<uint32_t, Job> jobs_;
    std::deque<uint32_t> pending_;
    std::deque<std::string> events_;
    uint32_t nextJob_;
    void unavailable();
    bool collectEvents();
    void executeJob();
    void pollOperationJob();
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
