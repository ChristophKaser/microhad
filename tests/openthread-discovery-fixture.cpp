// Probe-only accessory advertisement. Uses real signed SRP registration, never
// deposits a resolver address. This archive is not linked by thread-matter.
#include <openthread/instance.h>
#include <openthread/srp_client.h>
#include <openthread/srp_server.h>
#include <cstring>
extern "C" otInstance *gatewayOtDiscoveryTestInstance();
namespace {
char label[34];
otIp6Address address;
otSrpClientService service = {};
const uint8_t idle[] = {'5', '0', '0'};
const uint8_t active[] = {'3', '0', '0'};
const otDnsTxtEntry txt[] = {{"SII", idle, sizeof(idle)}, {"SAI", active, sizeof(active)}};
}
extern "C" int gatewayOtDiscoveryTestRegister(const char *name, const uint8_t *ip, uint16_t port)
{
    otInstance *instance = gatewayOtDiscoveryTestInstance();
    if (!instance || !name || std::strlen(name) != 33 || !ip || port == 0) return -OT_ERROR_INVALID_ARGS;
    std::memcpy(label, name, sizeof(label));
    std::memcpy(address.mFields.m8, ip, 16);
    std::memset(&service, 0, sizeof(service));
    service.mName = "_matter._tcp";
    service.mInstanceName = label;
    service.mPort = port;
    service.mTxtEntries = txt;
    service.mNumTxtEntries = sizeof(txt) / sizeof(txt[0]);
    otError error = otSrpClientSetHostName(instance, "synthetic-accessory");
    if (error == OT_ERROR_NONE) error = otSrpClientSetHostAddresses(instance, &address, 1);
    if (error == OT_ERROR_NONE) error = otSrpClientAddService(instance, &service);
    return -static_cast<int>(error);
}
extern "C" bool gatewayOtDiscoveryTestRegistered()
{
    return service.mState == OT_SRP_CLIENT_ITEM_STATE_REGISTERED;
}
