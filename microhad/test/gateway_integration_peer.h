#ifndef MICROHAD_ZIGBEE_GATEWAY_INTEGRATION_PEER_H
#define MICROHAD_ZIGBEE_GATEWAY_INTEGRATION_PEER_H

#include "gateway.h"

#include <cassert>
#include <ctime>
#include <fcntl.h>

class GatewayIntegrationTestPeer {
public:
    static void attachSerial(Gateway* gateway, int fd) {
        assert(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
        gateway->serialFd_ = fd;
    }
    static bool online(const Gateway& gateway) { return gateway.online_; }
    static void expireResetDeadline(Gateway* gateway) { gateway->resetDeadline_ = time(0) - 1; }
};

#endif
