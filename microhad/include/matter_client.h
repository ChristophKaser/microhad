#ifndef MICROHAD_MATTER_CLIENT_H
#define MICROHAD_MATTER_CLIENT_H

#include <string>
#include <stdint.h>
#include "matter_protocol.h"
#include <deque>

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
