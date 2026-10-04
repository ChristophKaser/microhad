#ifndef MICROHAD_MATTER_CLIENT_H
#define MICROHAD_MATTER_CLIENT_H

#include <string>
#include <stdint.h>
#include "matter_protocol.h"
#include <deque>
#include "gateway.h"

class MatterClient {
public:
    explicit MatterClient(const std::string& socketPath);
    bool status(bool thread, std::string* json, std::string* error) const;
    bool observe(unsigned char channel, bool online, std::string* json, std::string* error) const;
private:
    friend void runMatterClientUnitTests();
    std::string socketPath_;
    bool requestStatus(int fd, bool thread, std::string* json, std::string* error) const;
    bool call(unsigned char opcode, const std::string& body, std::string* json, std::string* error) const;
    bool exchange(int fd, unsigned char opcode, const std::string& body, int64_t deadline,
                  std::string* json, std::string* error) const;
};

// Single worker owner; no internal locking and no payload logging.
class MatterChannel {
public:
    explicit MatterChannel(const std::string& socketPath);
    ~MatterChannel();
    bool call(unsigned char opcode, const std::string& body, std::string* json, std::string* error);
    bool poll(std::string* error);
    static bool validRequest(unsigned char opcode, const std::string& body);
    static bool acceptedOperationJob(unsigned char opcode, const std::string& response, std::string* job);
    static bool operationStatus(const std::string& response, const std::string& job,
                                bool* terminal, bool* failed, std::string* error);
    static std::string operationPollRequest(const std::string& job);
    static bool threadMutation(bool importing, const std::string& input, std::string* body);
    static bool threadChannel(const std::string& input, unsigned char channel, std::string* body);
    static bool deviceReadRequest(const std::string& input, std::string* body);
    static bool deviceCommandRequest(const std::string& input, std::string* body);
    static bool devicePage(const std::string& response, std::string* revision, long* nextCursor,
                           std::vector<MatterEndpoint>* endpoints);
    static bool deviceResult(const std::string& response, const std::string& job, MatterReading* reading);
    static bool deviceReportEvent(const std::string& event, MatterReading* reading);
    static bool relayRequest(const std::string& input, uint32_t* id, unsigned char* opcode,
                             std::string* session, std::string* body);
    static std::string relayResponseSession(const std::string& response);
    static uint32_t relayGeneration(const std::string& response);
    static bool relayEvent(const std::string& input, const std::string& session, uint32_t generation);
    bool popEvent(std::string* json);
    void disconnect();
private:
    friend void runMatterClientUnitTests();
    MatterChannel(const MatterChannel&);
    MatterChannel& operator=(const MatterChannel&);
    std::string socketPath_;
    int fd_;
    uint32_t nextId_;
    MatterWire::Decoder decoder_;
    std::deque<std::string> events_;
    bool receive(int64_t deadline, bool wait);
    bool event(const MatterWire::Frame& frame);
};

#endif
