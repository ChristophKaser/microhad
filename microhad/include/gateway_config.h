#ifndef MICROHAD_ZIGBEE_GATEWAY_CONFIG_H
#define MICROHAD_ZIGBEE_GATEWAY_CONFIG_H

#include <string>
#include <map>

struct GatewayConfig {
    std::string zigguratSocket;
    std::string serialDevice;
    long serialBaud;
    long broadcastPassiveAckQuorum;
    std::string matterCountryCode;
    std::string matterRegulatoryLocation;
    std::string runtimeDirectory;
    std::string syslogRemote;
    long httpPort;
    long httpsPort;
    std::string firebaseServiceAccount;
    std::string firebaseTopic;
    std::string gatewayName;
    long companionDiscovery;
    std::string firebaseApplicationId;
    std::string firebaseApiKey;
    std::string firebaseProjectId;
    std::string firebaseSenderId;
    std::string companionJson() const;
    std::string caBundle;
    std::string tlsCertificate;
    std::string tlsPrivateKey;
    GatewayConfig();
};

bool selectGatewayConfigPath(int argc, const char* const argv[], std::string* path, std::string* error);
// Optional configuredValues returns explicitly configured fields as decoded
// strings (including decimal integers), without exporting implicit defaults.
bool loadGatewayConfig(const std::string& path, GatewayConfig* config, std::string* error,
                       std::map<std::string, std::string>* configuredValues = 0);

#endif
