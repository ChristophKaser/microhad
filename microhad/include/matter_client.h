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
    bool call(unsigned char opcode,const MatterProto::MessageBody& body,std::string* json,std::string* error) const;
    bool exchangeTyped(int fd,unsigned char opcode,const MatterProto::MessageBody&,int64_t deadline,std::string* json,std::string* error) const;
#ifdef MICROHAD_TESTING
    bool exchange(int fd, unsigned char opcode, const std::string& body, int64_t deadline,
                  std::string* json, std::string* error) const;
#endif
};

// Single worker owner; no internal locking and no payload logging.
class MatterChannel {
public:
    typedef MatterProto::MessageBody Body;
    bool call(unsigned char opcode,const Body& body,Body* response,std::string* error);
    bool popEvent(Body* event);
    static bool acceptedOperationJob(unsigned char,const Body&,uint64_t*);
    static bool operationStatus(const Body&,uint64_t,bool*,bool*,std::string*);
    static Body operationPollBody(uint64_t);
    static bool threadChannel(const Body&,unsigned char,Body*);
    static bool deviceResult(const Body&,uint64_t,MatterReading*);
    static bool inventoryNodesEvent(const Body&,std::vector<std::string>*);
    static bool inventoryEvent(const Body&,std::string*,unsigned int*,MatterEndpoint*);
    static bool deviceReportEvent(const Body&,MatterReading*);
    static bool deviceButtonEvent(const Body&,MatterButtonEvent*);
    static uint64_t relayResponseSession(const Body&);
    static uint32_t relayGeneration(const Body&);
    static bool relayEvent(const Body&,uint64_t,uint32_t);
    typedef bool (*EventHandler)(void* opaque);
    explicit MatterChannel(const std::string& socketPath);
    ~MatterChannel();
#ifdef MICROHAD_TESTING
    bool call(unsigned char opcode, const std::string& body, std::string* json, std::string* error);
#endif
    bool poll(std::string* error);
    void setEventHandler(EventHandler handler, void* opaque);
    static bool requestFromJson(unsigned char,const std::string&,Body*);
    static bool validRequest(unsigned char opcode, const std::string& body);
    static bool commissioningStartRequest(const std::string& input, const std::string& countryCode,
                                          const std::string& location, std::string* body);
#ifdef MICROHAD_TESTING
    static bool acceptedOperationJob(unsigned char opcode, const std::string& response, std::string* job);
#endif
#ifdef MICROHAD_TESTING
    static bool operationStatus(const std::string& response, const std::string& job,
                                bool* terminal, bool* failed, std::string* error);
#endif
#ifdef MICROHAD_TESTING
    static std::string operationPollRequest(const std::string& job);
#endif
    static bool threadMutation(const std::string& action, const std::string& input, std::string* body);
#ifdef MICROHAD_TESTING
    static bool threadChannel(const std::string& input, unsigned char channel, std::string* body);
#endif
    static bool deviceReadRequest(const std::string& input, std::string* body);
    static bool deviceCommandRequest(const std::string& input, std::string* body);
    static bool deviceSubscriptionsRequest(const std::string& input, std::string* body);
    static bool deviceBindingRequest(const std::string& input, std::string* body);
#ifdef MICROHAD_TESTING
    static bool deviceResult(const std::string& response, const std::string& job, MatterReading* reading);
#endif
#ifdef MICROHAD_TESTING
    static bool inventoryNodesEvent(const std::string& event, std::vector<std::string>* nodes);
#endif
#ifdef MICROHAD_TESTING
    static bool inventoryEvent(const std::string& event, std::string* kind, unsigned int* count, MatterEndpoint* endpoint);
#endif
#ifdef MICROHAD_TESTING
    static bool deviceReportEvent(const std::string& event, MatterReading* reading);
#endif
#ifdef MICROHAD_TESTING
    static bool deviceButtonEvent(const std::string& event, MatterButtonEvent* button);
#endif
    static bool relayRequest(const std::string& input, uint32_t* id, unsigned char* opcode,
                             std::string* session, std::string* body);
#ifdef MICROHAD_TESTING
    static std::string relayResponseSession(const std::string& response);
#endif
#ifdef MICROHAD_TESTING
    static uint32_t relayGeneration(const std::string& response);
#endif
#ifdef MICROHAD_TESTING
    static bool relayEvent(const std::string& input, const std::string& session, uint32_t generation);
#endif
#ifdef MICROHAD_TESTING
    bool popEvent(std::string* json);
#endif
    void disconnect();
private:
    friend void runMatterClientUnitTests();
    MatterChannel(const MatterChannel&);
    MatterChannel& operator=(const MatterChannel&);
    std::string socketPath_;
    int fd_;
    uint32_t nextId_;
    MatterWire::Decoder decoder_;
    std::deque<Body> events_;
    EventHandler eventHandler_;
    void* eventHandlerOpaque_;
    bool receive(int64_t deadline, bool wait);
    bool event(const MatterWire::Frame& frame);
};

#endif
