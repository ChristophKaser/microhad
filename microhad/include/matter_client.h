#ifndef MICROHAD_MATTER_CLIENT_H
#define MICROHAD_MATTER_CLIENT_H

#include <string>

class MatterClient {
public:
    explicit MatterClient(const std::string& socketPath);
    bool status(bool thread, std::string* json, std::string* error) const;
private:
    friend void runMatterClientUnitTests();
    std::string socketPath_;
    bool requestStatus(int fd, bool thread, std::string* json, std::string* error) const;
};

#endif
