#ifndef MICROHAD_ZIGBEE_DEVICE_SHELLY_H
#define MICROHAD_ZIGBEE_DEVICE_SHELLY_H

#include <string>
#include <vector>

struct ShellyDevice {
    std::string id, name, host, callbackHost, model, username, password, callbackToken;
    std::vector<int> inputIndices, temperatureIndices, relayIndices;
    unsigned int pollSeconds;
    bool invertContact;
    long nextPoll;
    bool manualRefresh;
    ShellyDevice();
};

namespace ShellyProtocol {
bool parsePowerStatus(const std::string& body, double* watts);
}

#endif
