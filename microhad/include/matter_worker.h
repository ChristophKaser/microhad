#ifndef MICROHAD_MATTER_WORKER_H
#define MICROHAD_MATTER_WORKER_H

#include "matter_client.h"
#include "matter_websocket.h"
#include <deque>
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
    std::string otaSocket() const { return otaSocket_; }
    bool status(bool thread, std::string* json, std::string* error);
    bool submit(unsigned char opcode,const MatterProto::MessageBody& body,uint32_t* id,std::string* error);
    bool job(uint32_t id, std::string* json, int accessRole = 2);
    bool takeEvent(MatterProto::MessageBody* event);
 #ifdef MICROHAD_TESTING
    bool takeEvent(std::string* json);
 #endif
    bool adoptRelay(int fd, const std::string& handshake, std::string* error, const std::string& initialInput = "");

private:
    std::string otaSocket_;
    GatewayRunner* gateway_;
    int phonePendingFd_;
    std::string phoneHandshake_;
    std::string phoneInitialInput_;
    bool phoneActive_;
    int phoneFd_;
    bool phoneReady_;
    MatterWebSocket::Decoder phoneDecoder_;
    std::deque<std::string> phoneOutput_;
    unsigned long phoneOffset_;
    int64_t phoneReceived_;
    int64_t phonePingAt_;
    int64_t phoneWriteSince_;
    uint32_t phoneLastId_;
    uint32_t phoneGeneration_;
    uint64_t phoneSession_;
    uint64_t abandonedSession_;
    bool phoneQueue(unsigned char opcode, const std::string& payload);
    void phonePump();
    bool continuingOverThread(uint64_t session);
    void dropPhone(bool cancel, unsigned int code);

    MatterChannel client_;
    struct Job {
        unsigned char opcode;
        int minimumRole;
        MatterProto::MessageBody body;
        std::string state;
        std::tr1::shared_ptr<const MatterProto::MessageBody> response;
        unsigned char responseOpcode;
        std::string error;
        uint64_t serviceJob;
        std::string lightLabel;
        std::string thermostatNode;
        std::string thermostatField;
        unsigned short thermostatEndpoint;
        int64_t pollAt;
        Job() : opcode(0), minimumRole(2), responseOpcode(0), serviceJob(0), thermostatEndpoint(0), pollAt(0) {}
    };
    std::map<uint32_t, Job> jobs_;
    std::deque<uint32_t> pending_;
    std::deque<MatterProto::MessageBody> events_;
    std::map<std::string, std::vector<MatterEndpoint> > inventoryBuilding_;
    std::vector<MatterEndpoint> snapshotDevices_;

    std::deque<MatterReading> deferredReadings_;
    std::map<std::string, time_t> deferredContacts_;
    uint32_t nextJob_;
    static bool drainMatterEvents(void* opaque);
    void unavailable();
    bool collectEvents();
    void applyOrDeferReading(const MatterReading& reading);
    void flushDeferredReadings();
    void executeJob();
    void pollOperationJob();
    pthread_mutex_t mutex_;
    pthread_t thread_;
    bool running_;
    bool started_;
    std::tr1::shared_ptr<const MatterProto::MessageBody> matterStatus_;
    std::tr1::shared_ptr<const MatterProto::MessageBody> threadStatus_;
    int64_t refreshed_;
    bool running();
    void run();
    static void* threadMain(void* opaque);
    MatterWorker(const MatterWorker&);
    MatterWorker& operator=(const MatterWorker&);
};
#endif
