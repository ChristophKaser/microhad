// Single-threaded C ABI for OpenThread's userspace UDP stack. Never owns the UART.
#include <openthread/openthread-system.h>
#include <openthread/dataset.h>
#include <openthread/dns_client.h>
#include <openthread/srp_server.h>
#include <openthread/srp_client.h>
#include <openthread/ip6.h>
#include <openthread/link.h>
#include <openthread/logging.h>
#include <openthread/platform/settings.h>
#include <openthread/message.h>
#include <openthread/netdata.h>
#include <openthread/border_router.h>
#include <openthread/platform/logging.h>
#include <openthread/tasklet.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>
#include <openthread/error.h>
#include <openthread/udp.h>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <strings.h>

extern "C" {
struct GatewayOtAddress { uint8_t ip[16]; uint16_t port; };
typedef void (*GatewayOtLog)(uint8_t, const char *);
typedef void (*GatewayOtReceive)(void *, const GatewayOtAddress *, const uint8_t *, uint16_t);
int gatewayOtConfigureDiagnosticRoutes(bool onMesh, bool external);
}
namespace {
otInstance *instance = nullptr;
void resetDiscovery();
void pollDiagnostics();
GatewayOtLog diagnosticLog = nullptr;
bool bindingDiagnostics = false;
bool diagnosticOnMesh = false;
bool diagnosticExternal = false;
otIp6Prefix diagnosticMeshPrefix = {};
otIp6Prefix diagnosticNat64Prefix = {};
otIp6Prefix diagnosticDefaultRoute = {};
void diagnostic(uint8_t level, const char *format, ...)
{
    if (!diagnosticLog) return;
    char buffer[384];
    va_list args;
    va_start(args, format);
    ::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    diagnosticLog(level, buffer);
}
void networkDataSnapshot()
{
    uint8_t bytes[255];
    uint8_t length = sizeof(bytes);
    otError error = otNetDataGet(instance, false, bytes, &length);
    diagnostic(2, "Thread network data version=%u stable_version=%u length=%u result=%s",
        otNetDataGetVersion(instance), otNetDataGetStableVersion(instance), length, otThreadErrorToString(error));
    if (error != OT_ERROR_NONE) return;
    // Full TLVs include SRP/DNS advertisements; never include the active dataset or keys.
    for (unsigned offset = 0; offset < length; offset += 48) {
        char hex[97];
        unsigned count = length - offset < 48 ? length - offset : 48;
        for (unsigned i = 0; i < count; ++i) ::snprintf(hex + 2 * i, 3, "%02x", bytes[offset + i]);
        diagnostic(2, "Thread network data offset=%u hex=%s", offset, hex);
    }
}
void logOffMeshPacket(otMessage *message, void *)
{
    const uint16_t length = otMessageGetLength(message);
    uint8_t header[44] = {};
    if (length < 40 || otMessageRead(message, 0, header, length < sizeof(header) ? length : sizeof(header)) < 40 ||
        (header[0] >> 4) != 6) {
        diagnostic(1, "Off-mesh IPv6 drop reason=invalid_header length=%u", length);
        otMessageFree(message);
        return;
    }
    otIp6Address source = {}, destination = {};
    std::memcpy(source.mFields.m8, header + 8, 16);
    std::memcpy(destination.mFields.m8, header + 24, 16);
    char sourceText[OT_IP6_ADDRESS_STRING_SIZE], destinationText[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&source, sourceText, sizeof(sourceText));
    otIp6AddressToString(&destination, destinationText, sizeof(destinationText));
    unsigned sourcePort = 0, destinationPort = 0;
    if (length >= 44 && (header[6] == 6 || header[6] == 17)) {
        sourcePort = (static_cast<unsigned>(header[40]) << 8) | header[41];
        destinationPort = (static_cast<unsigned>(header[42]) << 8) | header[43];
    }
    diagnostic(1, "Off-mesh IPv6 drop reason=no_upstream src=%s dst=%s next_header=%u ports=%u:%u length=%u",
        sourceText, destinationText, header[6], sourcePort, destinationPort, length);
    for (uint16_t offset = 0; offset < length; offset += 40) {
        uint8_t bytes[40];
        const uint16_t count = length - offset < sizeof(bytes) ? length - offset : sizeof(bytes);
        if (otMessageRead(message, offset, bytes, count) != count) {
            diagnostic(1, "Off-mesh IPv6 read_failed offset=%u length=%u", offset, count);
            break;
        }
        char hex[81];
        for (unsigned i = 0; i < count; ++i) ::snprintf(hex + 2 * i, 3, "%02x", bytes[i]);
        diagnostic(1, "Off-mesh IPv6 bytes offset=%u hex=%s", offset, hex);
    }
    otMessageFree(message);
}
void makeDiagnosticPrefixes()
{
    const otMeshLocalPrefix *mesh = otThreadGetMeshLocalPrefix(instance);
    std::memcpy(diagnosticMeshPrefix.mPrefix.mFields.m8, mesh->m8, 8);
    std::memcpy(diagnosticNat64Prefix.mPrefix.mFields.m8, mesh->m8, 8);
    diagnosticMeshPrefix.mPrefix.mFields.m8[6] ^= 1;
    diagnosticNat64Prefix.mPrefix.mFields.m8[6] ^= 2;
    diagnosticMeshPrefix.mLength = 64;
    diagnosticNat64Prefix.mLength = 96;
    diagnosticDefaultRoute.mLength = 0;
}
void clearDiagnosticRoutes()
{
    if (diagnosticOnMesh) (void)otBorderRouterRemoveOnMeshPrefix(instance, &diagnosticMeshPrefix);
    if (diagnosticExternal) {
        (void)otBorderRouterRemoveRoute(instance, &diagnosticDefaultRoute);
        (void)otBorderRouterRemoveRoute(instance, &diagnosticNat64Prefix);
        otIp6SetReceiveCallback(instance, nullptr, nullptr);
    }
    if ((diagnosticOnMesh || diagnosticExternal) && otThreadGetDeviceRole(instance) >= OT_DEVICE_ROLE_CHILD)
        (void)otBorderRouterRegister(instance);
    diagnosticOnMesh = diagnosticExternal = false;
}
void stateChanged(otChangedFlags flags, void *)
{
    if (flags & OT_CHANGED_THREAD_NETDATA) networkDataSnapshot();
    if (flags & OT_CHANGED_THREAD_ROLE) {
        diagnostic(2, "Thread role=%s rloc=0x%04x partition=%lu leader_weight=%u local_weight=%u",
            otThreadDeviceRoleToString(otThreadGetDeviceRole(instance)), otThreadGetRloc16(instance),
            static_cast<unsigned long>(otThreadGetPartitionId(instance)), otThreadGetLeaderWeight(instance),
            otThreadGetLocalLeaderWeight(instance));
        if ((diagnosticOnMesh || diagnosticExternal) && otThreadGetDeviceRole(instance) >= OT_DEVICE_ROLE_CHILD) {
            otError error = otBorderRouterRegister(instance);
            diagnostic(2, "Diagnostic Thread network data registration result=%s(%u)", otThreadErrorToString(error), error);
        }
    }
}
void neighborChanged(otNeighborTableEvent event, const otNeighborTableEntryInfo *entry)
{
    if (!entry || event > OT_NEIGHBOR_TABLE_EVENT_CHILD_MODE_CHANGED) return;
    const otChildInfo &child = entry->mInfo.mChild;
    diagnostic(2, "Thread child=%s rloc=0x%04x rx_idle=%u rssi=%d timeout=%lu",
        event == OT_NEIGHBOR_TABLE_EVENT_CHILD_ADDED ? "added" :
        event == OT_NEIGHBOR_TABLE_EVENT_CHILD_REMOVED ? "removed" : "mode_changed",
        child.mRloc16, child.mRxOnWhenIdle, child.mAverageRssi, static_cast<unsigned long>(child.mTimeout));
}
bool guardedSettings = false;
bool activeRestoreAllowed = true;
bool settingsReadError = false;
struct Slot { otUdpSocket socket; GatewayOtReceive callback; void *context; bool open; };
Slot slots[2] = {};
void receive(void *context, otMessage *message, const otMessageInfo *info)
{
    Slot *slot = static_cast<Slot *>(context);
    uint16_t offset = otMessageGetOffset(message);
    uint16_t length = otMessageGetLength(message) - offset;
    uint8_t bytes[1500];
    if (length > sizeof(bytes)) return;
    if (otMessageRead(message, offset, bytes, length) != length) return;
    GatewayOtAddress address = {};
    std::memcpy(address.ip, info->mPeerAddr.mFields.m8, 16);
    address.port = info->mPeerPort;
    slot->callback(slot->context, &address, bytes, length);
}
}
// Override the weak POSIX sink: SRP parser/signature/conflict failures happen
// before our update callback. Keep every SRP event visible at INFO, including
// expirations/removals and rejected updates; other stack details remain DEBUG.
extern "C" void otPlatLog(otLogLevel, otLogRegion, const char *format, ...)
{
    char buffer[384];
    va_list args;
    va_start(args, format);
    ::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    diagnostic(std::strstr(buffer, "Off-mesh IPv6") ? 1 :
        std::strstr(buffer, "SrpServer") || std::strstr(buffer, "RX fragment ") || std::strstr(buffer, "RX first fragment failed") ||
        std::strstr(buffer, "Dropping rx frag frame") ? 2 : 1, "OpenThread %s", buffer);
}
extern "C" void gatewayOtSetDiagnosticLog(GatewayOtLog callback) { diagnosticLog = callback; }
extern "C" void gatewayOtSetBindingDiagnostics(bool enabled)
{
    bindingDiagnostics = enabled;
    if (!instance) return;
    networkDataSnapshot();
    const otIpCounters *ip = otThreadGetIp6Counters(instance);
    const otMacCounters *mac = otLinkGetCounters(instance);
    diagnostic(2, "Binding Thread diagnostics=%s role=%s SRP_state=%d port=%u client_running=%u",
        enabled ? "enabled" : "finished", otThreadDeviceRoleToString(otThreadGetDeviceRole(instance)),
        otSrpServerGetState(instance), otSrpServerGetPort(instance), otSrpClientIsRunning(instance));
    diagnostic(2, "Binding IPv6 rx_ok=%lu rx_fail=%lu tx_ok=%lu tx_fail=%lu MAC_security_fail=%lu MAC_unknown_neighbor=%lu",
        static_cast<unsigned long>(ip->mRxSuccess), static_cast<unsigned long>(ip->mRxFailure),
        static_cast<unsigned long>(ip->mTxSuccess), static_cast<unsigned long>(ip->mTxFailure),
        static_cast<unsigned long>(mac->mRxErrSec), static_cast<unsigned long>(mac->mRxErrUnknownNeighbor));
}
extern "C" void otPlatReset(otInstance *) { std::abort(); }
extern "C" otError __real_otPlatSettingsGet(otInstance *, uint16_t, int, uint8_t *, uint16_t *);
extern "C" otError __wrap_otPlatSettingsGet(otInstance *aInstance, uint16_t key, int index,
                                            uint8_t *value, uint16_t *length)
{
    // Preserve network/frame-counter settings, but never let an unchecked saved
    // dataset select the shared radio channel during Instance::AfterInit().
    if (guardedSettings && (key == OT_SETTINGS_KEY_PENDING_DATASET ||
        (key == OT_SETTINGS_KEY_ACTIVE_DATASET && !activeRestoreAllowed))) return OT_ERROR_NOT_FOUND;
    uint16_t expected = length ? *length : 0;
    otError result = __real_otPlatSettingsGet(aInstance, key, index, value, length);
    if (guardedSettings && key == OT_SETTINGS_KEY_NETWORK_INFO && result == OT_ERROR_NONE &&
        value && length && *length != expected) settingsReadError = true;
    return result;
}
extern "C" int gatewayOtInit(const char *url)
{
    const char prefix[] = "spinel+hdlc+unix:///";
    if (instance || !url || std::strncmp(url, prefix, sizeof(prefix) - 1) != 0 ||
        !std::strchr(url, '?') || std::strcmp(std::strchr(url, '?'), "?no-reset") != 0) return -7;
    otPlatformConfig config = {};
    config.mInterfaceName = "lo";
    config.mCoprocessorUrls.mUrls[0] = url;
    config.mCoprocessorUrls.mNum = 1;
    config.mSpeedUpFactor = 1;
    instance = otSysInit(&config);
    if (instance) {
        // Instance construction resets POSIX's dynamic threshold to CRITICAL.
        // Set it after initialization so parser/rejection logs remain enabled.
        if (otSetLogLevel(instance, OT_LOG_LEVEL_INFO) != OT_ERROR_NONE) {
            otSysDeinit(); instance = nullptr; return -1;
        }
        otError error = otSetStateChangedCallback(instance, stateChanged, nullptr);
        if (error != OT_ERROR_NONE) { otSysDeinit(); instance = nullptr; return -static_cast<int>(error); }
        otThreadRegisterNeighborTableCallback(instance, neighborChanged);
    }
    return instance ? 0 : -1;
}
extern "C" int gatewayOtInitOnChannel(const char *url, uint8_t channel)
{
    if (instance || channel < 11 || channel > 26) return -7;
    guardedSettings = true;
    activeRestoreAllowed = false;
    settingsReadError = false;
    int result = gatewayOtInit(url);
    if (result != 0) { guardedSettings = false; activeRestoreAllowed = true; return result; }
    // Prefer this gateway's partition when a newly commissioned FTD briefly
    // starts a competing singleton partition. Its default FTD weight is 64.
    otThreadSetLocalLeaderWeight(instance, 72);
    diagnostic(2, "Thread local leader weight=%u", otThreadGetLocalLeaderWeight(instance));
    otError error = otLinkSetSupportedChannelMask(instance, uint32_t(1) << channel);
    if (error == OT_ERROR_NONE) error = otLinkSetChannel(instance, channel);
    if (settingsReadError && error == OT_ERROR_NONE) error = OT_ERROR_PARSE;
    if (error != OT_ERROR_NONE) {
        otSysDeinit(); instance = nullptr; guardedSettings = false; activeRestoreAllowed = true;
    }
    return -static_cast<int>(error);
}
extern "C" int gatewayOtPoll(uint32_t timeoutUs)
{
    if (!instance || timeoutUs > 100000) return -7;
    otTaskletsProcess(instance);
    otSysMainloopContext loop = {};
    loop.mMaxFd = -1;
    loop.mTimeout.tv_usec = timeoutUs;
    otSysMainloopUpdate(instance, &loop);
    int result = otSysMainloopPoll(&loop);
    if (result >= 0) { otSysMainloopProcess(instance, &loop); pollDiagnostics(); }
    else if (errno != EINTR) return -1;
    return 0;
}
extern "C" int gatewayOtSetDataset(const uint8_t *bytes, uint16_t length)
{
    if (!instance || !bytes || length == 0 || length > OT_OPERATIONAL_DATASET_MAX_LENGTH) return -7;
    otOperationalDatasetTlvs dataset = {};
    std::memcpy(dataset.mTlvs, bytes, length);
    dataset.mLength = static_cast<uint8_t>(length);
    // SetActiveTlvs saves then reads back its own record. The caller has
    // validated these TLVs; allow that read rather than suppressing it like
    // the unchecked boot cache (which would discard all but the timestamp).
    bool previousRestoreAllowed = activeRestoreAllowed;
    activeRestoreAllowed = true;
    otError error = otDatasetSetActiveTlvs(instance, &dataset);
    if (error != OT_ERROR_NONE) {
        activeRestoreAllowed = previousRestoreAllowed;
        return -static_cast<int>(error);
    }
    error = otIp6SetEnabled(instance, true);
    if (error != OT_ERROR_NONE) return -static_cast<int>(error);
    error = otThreadSetEnabled(instance, true);
    if (error != OT_ERROR_NONE) return -static_cast<int>(error);
    if (diagnosticOnMesh || diagnosticExternal)
        return gatewayOtConfigureDiagnosticRoutes(diagnosticOnMesh, diagnosticExternal);
    return 0;
}
extern "C" int gatewayOtDisable()
{
    if (!instance) return -7;
    otError first = otThreadSetEnabled(instance, false);
    otError second = otIp6SetEnabled(instance, false);
    return -static_cast<int>(first != OT_ERROR_NONE ? first : second);
}
extern "C" int gatewayOtConfigureDiagnosticRoutes(bool onMesh, bool external)
{
    if (!instance) return -OT_ERROR_INVALID_STATE;
    clearDiagnosticRoutes();
    if (!onMesh && !external) return 0;
    makeDiagnosticPrefixes();
    otError error = OT_ERROR_NONE;
    if (onMesh) {
        otBorderRouterConfig config = {};
        config.mPrefix = diagnosticMeshPrefix;
        config.mPreference = OT_ROUTE_PREFERENCE_LOW;
        config.mPreferred = true;
        config.mSlaac = true;
        config.mDefaultRoute = external;
        config.mOnMesh = true;
        config.mStable = true;
        error = otBorderRouterAddOnMeshPrefix(instance, &config);
        if (error != OT_ERROR_NONE) goto fail;
        diagnosticOnMesh = true;
        char prefix[OT_IP6_PREFIX_STRING_SIZE];
        otIp6PrefixToString(&diagnosticMeshPrefix, prefix, sizeof(prefix));
        diagnostic(2, "Diagnostic Thread on-mesh prefix=%s slaac=1 preferred=1 stable=1 default_route=%u", prefix, external);
    }
    if (external) {
        otExternalRouteConfig config = {};
        config.mPrefix = diagnosticDefaultRoute;
        config.mPreference = OT_ROUTE_PREFERENCE_MED;
        config.mStable = true;
        error = otBorderRouterAddRoute(instance, &config);
        if (error != OT_ERROR_NONE) goto fail;
        diagnosticExternal = true;
        config.mPrefix = diagnosticNat64Prefix;
        config.mPreference = OT_ROUTE_PREFERENCE_LOW;
        config.mNat64 = true;
        error = otBorderRouterAddRoute(instance, &config);
        if (error != OT_ERROR_NONE) goto fail;
        otIp6SetReceiveCallback(instance, logOffMeshPacket, nullptr);
        char prefix[OT_IP6_PREFIX_STRING_SIZE];
        otIp6PrefixToString(&diagnosticNat64Prefix, prefix, sizeof(prefix));
        diagnostic(2, "Diagnostic Thread external routes=::/0 and NAT64=%s capture=debug drop=no_upstream", prefix);
    }
    if (otThreadGetDeviceRole(instance) >= OT_DEVICE_ROLE_CHILD) {
        error = otBorderRouterRegister(instance);
        if (error != OT_ERROR_NONE) goto fail;
    }
    networkDataSnapshot();
    return 0;
fail:
    diagnostic(2, "Diagnostic Thread advertisement failed error=%s(%u)", otThreadErrorToString(error), error);
    clearDiagnosticRoutes();
    return -static_cast<int>(error);
}
extern "C" int gatewayOtOpen(uint16_t port, GatewayOtReceive callback, void *context)
{
    if (!instance || !callback || port == 0) return -7;
    for (int i = 0; i < 2; ++i) {
        Slot &slot = slots[i];
        if (slot.open) continue;
        std::memset(&slot, 0, sizeof(slot));
        slot.callback = callback;
        slot.context = context;
        otError error = otUdpOpen(instance, &slot.socket, receive, &slot);
        if (error != OT_ERROR_NONE) return -static_cast<int>(error);
        otSockAddr address = {};
        address.mPort = port;
        error = otUdpBind(instance, &slot.socket, &address, OT_NETIF_THREAD_INTERNAL);
        if (error != OT_ERROR_NONE) { (void)!otUdpClose(instance, &slot.socket); return -static_cast<int>(error); }
        slot.open = true;
        return i;
    }
    return -3;
}
extern "C" int gatewayOtClose(int id)
{
    if (!instance || id < 0 || id >= 2 || !slots[id].open) return -7;
    otError error = otUdpClose(instance, &slots[id].socket);
    if (error == OT_ERROR_NONE) slots[id].open = false;
    return -static_cast<int>(error);
}
extern "C" int gatewayOtSend(int id, const GatewayOtAddress *peer, const uint8_t *bytes, uint16_t length)
{
    if (!instance || id < 0 || id >= 2 || !slots[id].open || !peer || !bytes || length > 1232) return -7;
    otMessage *message = otUdpNewMessage(instance, nullptr);
    if (!message) return -3;
    otError error = otMessageAppend(message, bytes, length);
    otMessageInfo info = {};
    std::memcpy(info.mPeerAddr.mFields.m8, peer->ip, 16);
    info.mPeerPort = peer->port;
    info.mSockPort = slots[id].socket.mSockName.mPort;
    if (info.mSockPort == 5353) info.mHopLimit = 255;
    if (error == OT_ERROR_NONE) error = otUdpSend(instance, &slots[id].socket, message, &info);
    if (error != OT_ERROR_NONE) otMessageFree(message);
    return -static_cast<int>(error);
}
extern "C" int gatewayOtOwnAddress(GatewayOtAddress *address)
{
    if (!instance || !address) return -7;
    const otIp6Address *ip = otThreadGetMeshLocalEid(instance);
    std::memcpy(address->ip, ip->mFields.m8, 16);
    address->port = 0;
    return 0;
}
extern "C" int gatewayOtMulticast(const uint8_t *bytes, bool join)
{
    if (!instance || !bytes || bytes[0] != 0xff) return -7;
    otIp6Address address;
    std::memcpy(address.mFields.m8, bytes, 16);
    otError error = join ? otIp6SubscribeMulticastAddress(instance, &address) : otIp6UnsubscribeMulticastAddress(instance, &address);
    if (join && error == OT_ERROR_ALREADY) error = OT_ERROR_NONE;
    return -static_cast<int>(error);
}
extern "C" void gatewayOtDeinit()
{
    if (!instance) return;
    clearDiagnosticRoutes();
    for (int i = 0; i < 2; ++i) if (slots[i].open) gatewayOtClose(i);
    otSysDeinit();
    resetDiscovery();
    instance = nullptr;
    guardedSettings = false;
    activeRestoreAllowed = true;
    settingsReadError = false;
}

extern "C" bool gatewayOtReady() { return instance && otThreadGetDeviceRole(instance) >= OT_DEVICE_ROLE_CHILD; }

// Target libc wrappers are supplied by the shared Rust compatibility crate.

// DNS query contexts are static generation cookies, never pointers into a Rust future.
extern "C" {
struct GatewayOtDnsAnswer {
    uint8_t addresses[4][16];
    uint32_t ttl;
    uint16_t port;
    uint16_t txtLength;
    uint8_t count;
    uint8_t txt[256];
};
struct GatewayOtBrowseResult {
    uint16_t count;
    bool overflow;
    uint8_t reserved;
    uint64_t nodes[64];
};
}
static_assert(sizeof(GatewayOtDnsAnswer) == 332, "DNS ABI size");
namespace {
const char matterService[] = "_matter._tcp.default.service.arpa.";
struct DnsQuery {
    uint32_t token;
    bool pending;
    bool discarded;
    bool done;
    otError error;
    char label[34];
    GatewayOtDnsAnswer answer;
};
DnsQuery dnsQuery = {};
uint32_t nextDnsToken = 0;
struct BrowseQuery {
    uint32_t token;
    bool pending;
    bool discarded;
    bool done;
    otError error;
    uint64_t fabric;
    uint64_t nextNode;
    GatewayOtBrowseResult result;
};
BrowseQuery browseQuery = {};
uint32_t nextBrowseToken = 0;
otUdpReceiver dnsObserver = {};
otSrpServerResponseCounters lastSrpCounters = {};
int lastSrpState = -1;
uint16_t lastSrpPort = 0;
bool discoveryStarted = false;
void pollDiagnostics()
{
    if (!discoveryStarted) return;
    int state = otSrpServerGetState(instance);
    uint16_t port = otSrpServerGetPort(instance);
    if (state != lastSrpState || port != lastSrpPort) {
        diagnostic(2, "SRP server state=%d port=%u", state, port);
        lastSrpState = state; lastSrpPort = port;
    }
    const otSrpServerResponseCounters *c = otSrpServerGetResponseCounters(instance);
    if (c && std::memcmp(c, &lastSrpCounters, sizeof(*c)) != 0) {
        diagnostic(2, "SRP responses success=%lu failure=%lu format=%lu name_exists=%lu refused=%lu other=%lu",
            static_cast<unsigned long>(c->mSuccess), static_cast<unsigned long>(c->mServerFailure),
            static_cast<unsigned long>(c->mFormatError), static_cast<unsigned long>(c->mNameExists),
            static_cast<unsigned long>(c->mRefused), static_cast<unsigned long>(c->mOther));
        lastSrpCounters = *c;
    }
}
bool observeDns(void *, const otMessage *message, const otMessageInfo *info)
{
    // Passive metadata only: never consume or modify a protocol packet.
    bool srp = otSrpServerGetPort(instance) != 0 && info->mSockPort == otSrpServerGetPort(instance);
    if (!srp && info->mSockPort != 53 && info->mPeerPort != 53) return false;
    char peer[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&info->mPeerAddr, peer, sizeof(peer));
    uint8_t header[12];
    if (otMessageRead(message, otMessageGetOffset(message), header, sizeof(header)) != sizeof(header)) {
        diagnostic(srp || bindingDiagnostics ? 2 : 1, "DNS UDP RX short_header peer=[%s]:%u local=%u length=%u",
            peer, info->mPeerPort, info->mSockPort, otMessageGetLength(message) - otMessageGetOffset(message));
        return false;
    }
    diagnostic(srp || bindingDiagnostics ? 2 : 1, "DNS UDP RX peer=[%s]:%u local=%u id=%u opcode=%u response=%u rcode=%u",
        peer, info->mPeerPort, info->mSockPort, (header[0] << 8) | header[1], (header[2] >> 3) & 15,
        header[2] >> 7, header[3] & 15);
    return false;
}
void resetDiscovery() {
    discoveryStarted = false;
    bindingDiagnostics = false;
    dnsObserver = {};
    lastSrpCounters = {};
    lastSrpState = -1;
    lastSrpPort = 0;
    std::memset(&dnsQuery, 0, sizeof(dnsQuery));
    std::memset(&browseQuery, 0, sizeof(browseQuery));
}
void dnsBrowsed(otError error, const otDnsBrowseResponse *response, void *context)
{
    if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(context)) != browseQuery.token) return;
    browseQuery.pending = false;
    browseQuery.done = !browseQuery.discarded;
    browseQuery.error = error;
    if (browseQuery.discarded || error != OT_ERROR_NONE) return;
    char service[128] = {};
    error = otDnsBrowseResponseGetServiceName(response, service, sizeof(service));
    if (error != OT_ERROR_NONE || strcasecmp(service, matterService) != 0) {
        browseQuery.error = error == OT_ERROR_NONE ? OT_ERROR_PARSE : error;
        return;
    }
    bool ended = false;
    for (uint16_t index = 0; index < 256; ++index) {
        char label[64] = {};
        error = otDnsBrowseResponseGetServiceInstance(response, index, label, sizeof(label));
        if (error == OT_ERROR_NOT_FOUND) { ended = true; break; }
        if (error != OT_ERROR_NONE) { browseQuery.error = error; return; }
        if (std::strlen(label) != 33 || label[16] != '-') continue;
        bool hexLabel = true;
        for (unsigned i = 0; i < 33; ++i) {
            if (i == 16) continue;
            char ch = label[i];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') ||
                  (ch >= 'a' && ch <= 'f'))) hexLabel = false;
        }
        if (!hexLabel) continue;
        char fabricText[17] = {};
        char nodeText[17] = {};
        std::memcpy(fabricText, label, 16);
        std::memcpy(nodeText, label + 17, 16);
        char *end = nullptr;
        uint64_t fabric = ::strtoull(fabricText, &end, 16);
        if (!end || *end != '\0' || fabric != browseQuery.fabric) continue;
        uint64_t node = ::strtoull(nodeText, &end, 16);
        if (!end || *end != '\0' || node < 2 || node >= browseQuery.nextNode) continue;
        bool duplicate = false;
        for (uint16_t i = 0; i < browseQuery.result.count; ++i)
            if (browseQuery.result.nodes[i] == node) duplicate = true;
        if (duplicate) continue;
        if (browseQuery.result.count == 64) { browseQuery.result.overflow = true; continue; }
        browseQuery.result.nodes[browseQuery.result.count++] = node;
    }
    if (!ended) { browseQuery.error = OT_ERROR_NO_BUFS; return; }
    browseQuery.error = OT_ERROR_NONE;
}
void srpUpdate(otSrpServerServiceUpdateId id, const otSrpServerHost *updated, uint32_t, void *)
{
    // Count retained key-leases as well as live records; replacement is one host.
    unsigned hosts = 1;
    unsigned services = 0;
    uint8_t count = 0;
    const otIp6Address *addresses = otSrpServerHostGetAddresses(updated, &count);
    otSrpServerLeaseInfo lease = {};
    otSrpServerHostGetLeaseInfo(updated, &lease);
    diagnostic(2, "SRP update id=%lu host=%s deleted=%u addresses=%u lease_ms=%lu key_lease_ms=%lu",
        static_cast<unsigned long>(id), otSrpServerHostGetFullName(updated), otSrpServerHostIsDeleted(updated), count,
        static_cast<unsigned long>(lease.mLease), static_cast<unsigned long>(lease.mKeyLease));
    for (uint8_t i = 0; i < count; ++i) {
        char address[OT_IP6_ADDRESS_STRING_SIZE];
        otIp6AddressToString(&addresses[i], address, sizeof(address));
        diagnostic(2, "SRP address id=%lu index=%u address=%s", static_cast<unsigned long>(id), i, address);
    }
    otError error = count <= 8 ? OT_ERROR_NONE : OT_ERROR_NO_BUFS;
    for (const otSrpServerHost *host = nullptr; (host = otSrpServerGetNextHost(instance, host)) != nullptr;) {
        if (strcasecmp(otSrpServerHostGetFullName(host), otSrpServerHostGetFullName(updated)) == 0) continue;
        ++hosts;
        for (const otSrpServerService *service = nullptr; (service = otSrpServerHostGetNextService(host, service)) != nullptr;) ++services;
    }
    unsigned added = 0;
    for (const otSrpServerService *service = nullptr; (service = otSrpServerHostGetNextService(updated, service)) != nullptr;) {
        ++added;
        diagnostic(2, "SRP service id=%lu instance=%s deleted=%u port=%u",
            static_cast<unsigned long>(id), otSrpServerServiceGetInstanceName(service),
            otSrpServerServiceIsDeleted(service), otSrpServerServiceGetPort(service));
        uint16_t length = 0;
        otSrpServerServiceGetTxtData(service, &length);
        if (length > 256) {
            diagnostic(2, "SRP rejection id=%lu reason=TXT_limit length=%u limit=256", static_cast<unsigned long>(id), length);
            error = OT_ERROR_NO_BUFS;
        }
    }
    if (hosts > 64 || added > 8 || services + added > 128) error = OT_ERROR_NO_BUFS;
    diagnostic(2, "SRP update decision id=%lu hosts=%u/64 addresses=%u/8 services=%u/8 total_services=%u/128 result=%s(%u)",
        static_cast<unsigned long>(id), hosts, count, added, services + added, otThreadErrorToString(error), error);
    otSrpServerHandleServiceUpdateResult(instance, id, error);
}
void dnsResolved(otError error, const otDnsServiceResponse *response, void *context)
{
    if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(context)) != dnsQuery.token) return;
    dnsQuery.pending = false;
    dnsQuery.done = !dnsQuery.discarded;
    dnsQuery.error = error;
    if (dnsQuery.discarded || error != OT_ERROR_NONE) {
        diagnostic(bindingDiagnostics ? 2 : 1, "DNS result token=%lu label=%s error=%s(%u) discarded=%u",
            static_cast<unsigned long>(dnsQuery.token), dnsQuery.label, otThreadErrorToString(error), error, dnsQuery.discarded);
        return;
    }
    char label[64] = {};
    char name[128] = {};
    char host[256] = {};
    error = otDnsServiceResponseGetServiceName(response, label, sizeof(label), name, sizeof(name));
    if (error == OT_ERROR_NONE && (strcasecmp(label, dnsQuery.label) != 0 || strcasecmp(name, matterService) != 0)) error = OT_ERROR_PARSE;
    otDnsServiceInfo info = {};
    info.mHostNameBuffer = host;
    info.mHostNameBufferSize = sizeof(host);
    info.mTxtData = dnsQuery.answer.txt;
    info.mTxtDataSize = sizeof(dnsQuery.answer.txt);
    if (error == OT_ERROR_NONE) error = otDnsServiceResponseGetServiceInfo(response, &info);
    if (error == OT_ERROR_NONE && (info.mTxtDataTruncated || info.mPort == 0 || info.mTtl == 0)) error = OT_ERROR_PARSE;
    if (error == OT_ERROR_NONE) {
        dnsQuery.answer.port = info.mPort;
        dnsQuery.answer.ttl = info.mTtl;
        dnsQuery.answer.txtLength = info.mTxtDataSize;
        if (info.mTxtDataSize && info.mTxtDataTtl < dnsQuery.answer.ttl) dnsQuery.answer.ttl = info.mTxtDataTtl;
        for (uint16_t i = 0; i < 4; ++i) {
            otIp6Address address;
            uint32_t ttl = 0;
            otError item = otDnsServiceResponseGetHostAddress(response, host, i, &address, &ttl);
            if (item == OT_ERROR_NOT_FOUND) break;
            if (item != OT_ERROR_NONE) { error = item; break; }
            std::memcpy(dnsQuery.answer.addresses[dnsQuery.answer.count++], address.mFields.m8, 16);
            if (ttl < dnsQuery.answer.ttl) dnsQuery.answer.ttl = ttl;
        }
        if (dnsQuery.answer.count == 0 || dnsQuery.answer.ttl == 0) error = OT_ERROR_NOT_FOUND;
    }
    dnsQuery.error = error;
    diagnostic(bindingDiagnostics ? 2 : 1, "DNS result token=%lu label=%s error=%s(%u) addresses=%u port=%u ttl=%lu",
        static_cast<unsigned long>(dnsQuery.token), dnsQuery.label, otThreadErrorToString(error), error,
        dnsQuery.answer.count, dnsQuery.answer.port, static_cast<unsigned long>(dnsQuery.answer.ttl));
}
}
extern "C" int gatewayOtDiscoveryStart()
{
    if (!instance) return -OT_ERROR_INVALID_STATE;
    otError error = otSrpServerSetAddressMode(instance, OT_SRP_SERVER_ADDRESS_MODE_UNICAST);
    if (error != OT_ERROR_NONE) return -static_cast<int>(error);
    otSrpServerSetServiceUpdateHandler(instance, srpUpdate, nullptr);
    otSrpServerSetEnabled(instance, true);
    // Auto-start selects an advertised SRP server and also updates the DNS client's
    // default address. No local host or service is registered by the production client.
    otSrpClientEnableAutoStartMode(instance, nullptr, nullptr);
    dnsObserver.mHandler = observeDns;
    error = otUdpAddReceiver(instance, &dnsObserver);
    if (error != OT_ERROR_NONE) return -static_cast<int>(error);
    discoveryStarted = true;
    diagnostic(2, "Thread discovery enabled domain=%s", otSrpServerGetDomain(instance));
    pollDiagnostics();
    return 0;
}
extern "C" int gatewayOtDnsStart(const char *label, uint32_t *token)
{
    if (!instance || !label || !token || std::strlen(label) != 33 || label[16] != '-') return -OT_ERROR_INVALID_ARGS;
    for (unsigned i = 0; i < 33; ++i) {
        if (i == 16) continue;
        if (!((label[i] >= '0' && label[i] <= '9') || (label[i] >= 'A' && label[i] <= 'F'))) return -OT_ERROR_INVALID_ARGS;
    }
    if (dnsQuery.pending || dnsQuery.done) return -OT_ERROR_BUSY;
    if (!otSrpClientIsRunning(instance)) return -OT_ERROR_INVALID_STATE;
    if (nextDnsToken == UINT32_MAX) return -OT_ERROR_NO_BUFS;
    std::memset(&dnsQuery, 0, sizeof(dnsQuery));
    dnsQuery.token = ++nextDnsToken;
    std::memcpy(dnsQuery.label, label, 34);
    otDnsQueryConfig config = *otDnsClientGetDefaultConfig(instance);
    char server[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&config.mServerSockAddr.mAddress, server, sizeof(server));
    diagnostic(bindingDiagnostics ? 2 : 1, "DNS query token=%lu label=%s server=[%s]:%u",
        static_cast<unsigned long>(dnsQuery.token), label, server, config.mServerSockAddr.mPort);
    config.mResponseTimeout = 1000;
    config.mMaxTxAttempts = 2;
    config.mServiceMode = OT_DNS_SERVICE_MODE_SRV_TXT;
    config.mTransportProto = OT_DNS_TRANSPORT_UDP;
    config.mNat64Mode = OT_DNS_NAT64_DISALLOW;
    dnsQuery.pending = true;
    otError error = otDnsClientResolveServiceAndHostAddress(instance, label, matterService, dnsResolved,
        reinterpret_cast<void *>(static_cast<uintptr_t>(dnsQuery.token)), &config);
    if (error != OT_ERROR_NONE) { dnsQuery.pending = false; return -static_cast<int>(error); }
    *token = dnsQuery.token;
    return 0;
}
extern "C" int gatewayOtDnsPoll(uint32_t token, GatewayOtDnsAnswer *answer)
{
    if (!instance || token == 0 || token != dnsQuery.token || dnsQuery.discarded || !answer) return -OT_ERROR_INVALID_STATE;
    if (dnsQuery.pending) return 1;
    if (!dnsQuery.done) return -OT_ERROR_INVALID_STATE;
    dnsQuery.done = false;
    if (dnsQuery.error != OT_ERROR_NONE) return -static_cast<int>(dnsQuery.error);
    *answer = dnsQuery.answer;
    return 0;
}
extern "C" void gatewayOtDnsRelease(uint32_t token)
{
    if (token == dnsQuery.token) {
        if (dnsQuery.pending) diagnostic(bindingDiagnostics ? 2 : 1, "DNS query cancelled while pending token=%lu label=%s", static_cast<unsigned long>(token), dnsQuery.label);
        dnsQuery.discarded = true; dnsQuery.done = false;
    }
}
extern "C" int gatewayOtBrowseStart(uint64_t fabric, uint64_t nextNode, uint32_t *token)
{
    if (!instance || !fabric || nextNode < 2 || !token) return -OT_ERROR_INVALID_ARGS;
    if (browseQuery.pending || browseQuery.done) return -OT_ERROR_BUSY;
    if (nextBrowseToken == UINT32_MAX) return -OT_ERROR_NO_BUFS;
    std::memset(&browseQuery, 0, sizeof(browseQuery));
    browseQuery.token = ++nextBrowseToken;
    browseQuery.fabric = fabric;
    browseQuery.nextNode = nextNode;
    otDnsQueryConfig config = *otDnsClientGetDefaultConfig(instance);
    config.mResponseTimeout = 1000;
    config.mMaxTxAttempts = 2;
    config.mTransportProto = OT_DNS_TRANSPORT_UDP;
    config.mNat64Mode = OT_DNS_NAT64_DISALLOW;
    browseQuery.pending = true;
    otError error = otDnsClientBrowse(instance, matterService, dnsBrowsed,
        reinterpret_cast<void *>(static_cast<uintptr_t>(browseQuery.token)), &config);
    if (error != OT_ERROR_NONE) { browseQuery.pending = false; return -static_cast<int>(error); }
    *token = browseQuery.token;
    return 0;
}
extern "C" int gatewayOtBrowsePoll(uint32_t token, GatewayOtBrowseResult *result)
{
    if (!instance || token == 0 || token != browseQuery.token || browseQuery.discarded || !result)
        return -OT_ERROR_INVALID_STATE;
    if (browseQuery.pending) return 1;
    if (!browseQuery.done) return -OT_ERROR_INVALID_STATE;
    browseQuery.done = false;
    if (browseQuery.error != OT_ERROR_NONE) return -static_cast<int>(browseQuery.error);
    if (browseQuery.result.overflow) return -OT_ERROR_NO_BUFS;
    *result = browseQuery.result;
    return 0;
}
extern "C" void gatewayOtBrowseRelease(uint32_t token)
{
    if (token == browseQuery.token) { browseQuery.discarded = true; browseQuery.done = false; }
}
// Only referenced by the separate probe fixture archive; linker GC removes it
// from the production service. There is no IPC or Rust production accessor.
extern "C" otInstance *gatewayOtDiscoveryTestInstance() { return instance; }
