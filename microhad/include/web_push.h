#ifndef MICROHAD_ZIGBEE_WEB_PUSH_H
#define MICROHAD_ZIGBEE_WEB_PUSH_H

#include <string>

class WebPushStore {
public:
    explicit WebPushStore(const std::string& runtimeDirectory);
    bool addSubscription(const std::string& subscriptionJson, std::string* error);
    std::string publicKey() const;
private:
    std::string runtimeDirectory_;
    std::string publicKey_;
    bool loadOrCreateKey(std::string* error);
};

#endif
