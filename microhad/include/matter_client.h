#ifndef MICROHAD_MATTER_CLIENT_H
#define MICROHAD_MATTER_CLIENT_H

#include <string>
#include <stdint.h>

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

#endif
