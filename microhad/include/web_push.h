#ifndef MICROHAD_ZIGBEE_WEB_PUSH_H
#define MICROHAD_ZIGBEE_WEB_PUSH_H

#include <string>
#include <vector>

class WebPushStore {
public:
    struct RoutingClient { std::string id, presenceSensorId; bool enabled; };
    explicit WebPushStore(const std::string& runtimeDirectory);
    bool addSubscription(const std::string& subscriptionJson, std::string* error);
    std::string clientsJson() const;
    void routingClients(std::vector<RoutingClient>* clients) const;
    bool updateClient(const std::string& endpoint, const std::string& label, bool enabled,
                      const std::string& presenceSensorId, std::string* error);
    bool updateClient(const std::string& endpoint, const std::string& label, bool enabled, std::string* error);
    bool removeClient(const std::string& endpoint, std::string* error);
    std::string publicKey() const;
    bool send(const std::string& caBundle, const std::string& title, const std::string& body,
              const std::vector<std::string>& allowedClientIds, std::string* error);
    bool send(const std::string& caBundle, const std::string& title, const std::string& body, std::string* error);
private:
    std::string runtimeDirectory_;
    std::string publicKey_;
    bool loadOrCreateKey(std::string* error);
};

#endif
