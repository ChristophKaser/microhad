#ifndef MICROHAD_ZIGBEE_WEB_PUSH_H
#define MICROHAD_ZIGBEE_WEB_PUSH_H

#include <string>

class WebPushStore {
public:
    explicit WebPushStore(const std::string& runtimeDirectory);
    bool addSubscription(const std::string& subscriptionJson, std::string* error);
    std::string clientsJson() const;
    bool updateClient(const std::string& endpoint, const std::string& label, bool enabled, std::string* error);
    bool removeClient(const std::string& endpoint, std::string* error);
    std::string publicKey() const;
    bool send(const std::string& caBundle, const std::string& title, const std::string& body, std::string* error);
private:
    std::string runtimeDirectory_;
    std::string publicKey_;
    bool loadOrCreateKey(std::string* error);
};

#endif
