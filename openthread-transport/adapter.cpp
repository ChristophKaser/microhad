// Single-threaded C ABI for OpenThread's userspace UDP stack. Never owns the UART.
#include <openthread/openthread-system.h>
#include <openthread/dataset.h>
#include <openthread/dns_client.h>
#include <openthread/srp_server.h>
#include <openthread/srp_client.h>
#include <openthread/ip6.h>
#include <openthread/link.h>
#include <openthread/platform/settings.h>
#include <openthread/message.h>
#include <openthread/tasklet.h>
#include <openthread/thread.h>
#include <openthread/udp.h>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <strings.h>

extern "C" {
struct GatewayOtAddress { uint8_t ip[16]; uint16_t port; };
typedef void (*GatewayOtReceive)(void *, const GatewayOtAddress *, const uint8_t *, uint16_t);
}
namespace {
otInstance *instance = nullptr;
void resetDiscovery();
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
    if (result >= 0) otSysMainloopProcess(instance, &loop);
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
    return -static_cast<int>(otThreadSetEnabled(instance, true));
}
extern "C" int gatewayOtDisable()
{
    if (!instance) return -7;
    otError first = otThreadSetEnabled(instance, false);
    otError second = otIp6SetEnabled(instance, false);
    return -static_cast<int>(first != OT_ERROR_NONE ? first : second);
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
    for (int i = 0; i < 2; ++i) if (slots[i].open) gatewayOtClose(i);
    otSysDeinit();
    resetDiscovery();
    instance = nullptr;
    guardedSettings = false;
    activeRestoreAllowed = true;
    settingsReadError = false;
}

extern "C" bool gatewayOtReady() { return instance && otThreadGetDeviceRole(instance) >= OT_DEVICE_ROLE_CHILD; }

#ifdef GATEWAY_OT_FACTORY_UCLIBC
// Factory uClibc 0.9.30 lacks these newer POSIX symbols used by Rust std.
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
// The service is single-threaded, so setting descriptor flags immediately
// after accept preserves accept4 semantics without a concurrent fork window.
extern "C" int accept4(int socket, struct sockaddr *address, socklen_t *length, int flags)
{
    if (flags & ~(O_NONBLOCK | O_CLOEXEC)) { errno = EINVAL; return -1; }
    int fd = accept(socket, address, length);
    if (fd < 0) return fd;
    if (((flags & O_NONBLOCK) && fcntl(fd, F_SETFL, O_NONBLOCK) < 0) ||
        ((flags & O_CLOEXEC) && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}
extern "C" int pthread_setname_np(pthread_t, const char *) { return ENOSYS; }
extern "C" int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                                struct timespec *remaining)
{
    if (!request || request->tv_sec < 0 || request->tv_nsec < 0 || request->tv_nsec >= 1000000000L ||
        (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME) || (flags != 0 && flags != TIMER_ABSTIME))
        return EINVAL;
    struct timespec delay = *request;
    // Rust std uses relative CLOCK_MONOTONIC sleeps. Absolute realtime deadlines
    // need clock-adjustment semantics that a nanosleep wrapper cannot provide.
    if (flags == TIMER_ABSTIME) return ENOTSUP;
    if (nanosleep(&delay, flags == 0 ? remaining : nullptr) == 0) return 0;
    return errno;
}
#endif

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
void resetDiscovery() { std::memset(&dnsQuery, 0, sizeof(dnsQuery)); }
void srpUpdate(otSrpServerServiceUpdateId id, const otSrpServerHost *updated, uint32_t, void *)
{
    // Count retained key-leases as well as live records; replacement is one host.
    unsigned hosts = 1;
    unsigned services = 0;
    uint8_t count = 0;
    otSrpServerHostGetAddresses(updated, &count);
    otError error = count <= 8 ? OT_ERROR_NONE : OT_ERROR_NO_BUFS;
    for (const otSrpServerHost *host = nullptr; (host = otSrpServerGetNextHost(instance, host)) != nullptr;) {
        if (strcasecmp(otSrpServerHostGetFullName(host), otSrpServerHostGetFullName(updated)) == 0) continue;
        ++hosts;
        for (const otSrpServerService *service = nullptr; (service = otSrpServerHostGetNextService(host, service)) != nullptr;) ++services;
    }
    unsigned added = 0;
    for (const otSrpServerService *service = nullptr; (service = otSrpServerHostGetNextService(updated, service)) != nullptr;) {
        ++added;
        uint16_t length = 0;
        otSrpServerServiceGetTxtData(service, &length);
        if (length > 256) error = OT_ERROR_NO_BUFS;
    }
    if (hosts > 64 || added > 8 || services + added > 128) error = OT_ERROR_NO_BUFS;
    otSrpServerHandleServiceUpdateResult(instance, id, error);
}
void dnsResolved(otError error, const otDnsServiceResponse *response, void *context)
{
    if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(context)) != dnsQuery.token) return;
    dnsQuery.pending = false;
    dnsQuery.done = !dnsQuery.discarded;
    dnsQuery.error = error;
    if (dnsQuery.discarded || error != OT_ERROR_NONE) return;
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
    if (token == dnsQuery.token) { dnsQuery.discarded = true; dnsQuery.done = false; }
}
// Only referenced by the separate probe fixture archive; linker GC removes it
// from the production service. There is no IPC or Rust production accessor.
extern "C" otInstance *gatewayOtDiscoveryTestInstance() { return instance; }
