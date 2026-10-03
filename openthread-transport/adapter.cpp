// Single-threaded C ABI for OpenThread's userspace UDP stack. Never owns the UART.
#include <openthread/openthread-system.h>
#include <openthread/dataset.h>
#include <openthread/ip6.h>
#include <openthread/message.h>
#include <openthread/tasklet.h>
#include <openthread/thread.h>
#include <openthread/udp.h>
#include <cstdlib>
#include <cstring>
#include <cerrno>

extern "C" {
struct GatewayOtAddress { uint8_t ip[16]; uint16_t port; };
typedef void (*GatewayOtReceive)(void *, const GatewayOtAddress *, const uint8_t *, uint16_t);
}
namespace {
otInstance *instance = nullptr;
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
    otError error = otDatasetSetActiveTlvs(instance, &dataset);
    if (error != OT_ERROR_NONE) return -static_cast<int>(error);
    error = otIp6SetEnabled(instance, true);
    if (error != OT_ERROR_NONE) return -static_cast<int>(error);
    return -static_cast<int>(otThreadSetEnabled(instance, true));
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
    instance = nullptr;
}

extern "C" bool gatewayOtReady() { return instance && otThreadGetDeviceRole(instance) >= OT_DEVICE_ROLE_CHILD; }

#ifdef GATEWAY_OT_FACTORY_UCLIBC
// Factory uClibc 0.9.30 lacks these newer POSIX symbols used by Rust std.
#include <pthread.h>
#include <time.h>
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
