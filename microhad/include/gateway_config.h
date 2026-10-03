#ifndef MICROHAD_ZIGBEE_GATEWAY_CONFIG_H
#define MICROHAD_ZIGBEE_GATEWAY_CONFIG_H

#include <string>

struct GatewayConfig {
    std::string zigguratSocket;
    std::string serialDevice;
    long serialBaud;
    std::string runtimeDirectory;
    long httpPort;
    long httpsPort;
    std::string firebaseServiceAccount;
    std::string firebaseTopic;
    std::string caBundle;
    std::string tlsCertificate;
    std::string tlsPrivateKey;
    GatewayConfig();
};

bool selectGatewayConfigPath(int argc, const char* const argv[], std::string* path, std::string* error);
bool loadGatewayConfig(const std::string& path, GatewayConfig* config, std::string* error);

#endif
