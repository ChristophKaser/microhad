// Exercise the actual OpenThread HDLC transport against a local stream socket.
#include "posix/platform/hdlc_interface.hpp"
#include "posix/platform/radio_url.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)
extern "C" void otPlatReset(otInstance *) { std::abort(); }
static void received(void *context) { ++*static_cast<unsigned *>(context); }
int main(void)
{
    alarm(10);
    char directory[] = "/tmp/ot-unix-test-XXXXXX";
    CHECK(mkdtemp(directory) != nullptr);
    char path[108];
    std::snprintf(path, sizeof(path), "%s/radio.sock", directory);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path);
    CHECK(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    CHECK(listen(listener, 1) == 0);
    char url[256];
    std::snprintf(url, sizeof(url), "spinel+hdlc+unix://%s", path);
    ot::Posix::RadioUrl missingReset(url);
    ot::Posix::HdlcInterface rejected(missingReset);
    ot::Spinel::SpinelInterface::RxFrameBuffer buffer;
    unsigned callbacks = 0;
    CHECK(rejected.Init(received, &callbacks, buffer) == OT_ERROR_INVALID_ARGS);
    std::strcat(url, "?no-reset");
    ot::Posix::RadioUrl radioUrl(url);
    ot::Posix::HdlcInterface transport(radioUrl);
    CHECK(transport.Init(received, &callbacks, buffer) == OT_ERROR_NONE);
    int peer = accept(listener, nullptr, nullptr);
    CHECK(peer >= 0);
    const uint8_t reset[] = {0x81, 1};
    CHECK(transport.SendFrame(reset, sizeof(reset)) == OT_ERROR_INVALID_STATE);
    const uint8_t untrackedReset[] = {0x80, 1};
    CHECK(transport.SendFrame(untrackedReset, sizeof(untrackedReset)) == OT_ERROR_INVALID_STATE);
    // Include every HDLC escape character and demand an exact decoded round trip.
    const uint8_t request[] = {0x81, 2, 0x7e, 0x7d, 0x11, 0x13, 0xf8};
    CHECK(transport.SendFrame(request, sizeof(request)) == OT_ERROR_NONE);
    uint8_t wire[128];
    ssize_t length = read(peer, wire, sizeof(wire));
    CHECK(length > 0);
    // Fragment the frame to verify the decoder retains state across reads.
    CHECK(write(peer, wire, 2) == 2);
    CHECK(transport.WaitForFrame(100000) == OT_ERROR_NONE);
    CHECK(callbacks == 0);
    CHECK(write(peer, wire + 2, length - 2) == length - 2);
    CHECK(transport.WaitForFrame(100000) == OT_ERROR_NONE);
    CHECK(callbacks == 1);
    CHECK(buffer.GetLength() == sizeof(request));
    CHECK(std::memcmp(buffer.GetFrame(), request, sizeof(request)) == 0);
    // Deinit must not wait for unrelated children (the UART/PTY close path does).
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) { sleep(20); _exit(0); }
    transport.Deinit();
    CHECK(waitpid(child, nullptr, WNOHANG) == 0);
    kill(child, SIGTERM);
    CHECK(waitpid(child, nullptr, 0) == child);
    close(peer);
    close(listener);
    unlink(path);
    rmdir(directory);
    std::puts("Unix HDLC adapter: reset guard, escaped/fragmented round trip, independent close passed");
}
