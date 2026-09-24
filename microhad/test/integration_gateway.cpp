#include "ash.h"
#include "gateway.h"
#include "gateway_integration_peer.h"

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
std::vector<unsigned char> readAvailable(int fd) {
    std::vector<unsigned char> bytes;
    unsigned char buffer[256];
    ssize_t count;
    assert(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
    while ((count = read(fd, buffer, sizeof(buffer))) > 0)
        bytes.insert(bytes.end(), buffer, buffer + count);
    return bytes;
}

void testResetAndVersionOverPipe() {
    int serial[2];
    std::string error;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    assert(gateway.start(&error));
    std::vector<unsigned char> reset = readAvailable(serial[1]);
    assert(!reset.empty() && reset[0] == 0x1a && reset[1] == 0xc0);
    const unsigned char acknowledgement[] = {0x1a, 0xc1, 0x02, 0x0b, 0x0a, 0x52, 0x7e};
    assert(write(serial[1], acknowledgement, sizeof(acknowledgement)) == static_cast<ssize_t>(sizeof(acknowledgement)));
    gateway.poll();
    std::vector<unsigned char> versionRequest = readAvailable(serial[1]);
    assert(!versionRequest.empty());
    std::vector<unsigned char> versionReply;
    versionReply.push_back(0); versionReply.push_back(0x80); versionReply.push_back(0);
    versionReply.push_back(8); versionReply.push_back(2); versionReply.push_back(0x80); versionReply.push_back(0x67);
    std::vector<unsigned char> wire = AshTransport::encodeData(0x01, versionReply);
    assert(write(serial[1], &wire[0], wire.size()) == static_cast<ssize_t>(wire.size()));
    gateway.poll();
    std::vector<unsigned char> negotiatedRequest = readAvailable(serial[1]);
    assert(!negotiatedRequest.empty());
    /* The second request is the negotiated protocol-version exchange.  Its
       exact response variants are covered by unit_gateway; this integration
       test verifies that the live serial path reaches that exchange. */
    gateway.stop();
    close(serial[1]);
}

void testResetTimeoutRetriesOverPipe() {
    int serial[2];
    std::string error;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    assert(gateway.start(&error));
    std::vector<unsigned char> initial = readAvailable(serial[1]);
    assert(!initial.empty() && initial[0] == 0x1a && initial[1] == 0xc0);
    GatewayIntegrationTestPeer::expireResetDeadline(&gateway);
    gateway.poll();
    std::vector<unsigned char> retry = readAvailable(serial[1]);
    assert(!retry.empty() && retry[0] == 0x1a && retry[1] == 0xc0);
    assert(!GatewayIntegrationTestPeer::online(gateway));
    gateway.stop();
    close(serial[1]);
}

void testRuntimeDirectoryBoundaries() {
    char name[] = "/tmp/microhad-zigbee-runtime-XXXXXX";
    char* temporary = mkdtemp(name);
    std::string error;
    int serial[2];
    assert(temporary != 0);
    std::string directory = temporary;
    assert(rmdir(directory.c_str()) == 0); /* absent at startup */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    {
        Gateway gateway("/dev/null", 115200, directory, "", "", "");
        GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
        assert(gateway.start(&error));
        struct stat details;
        assert(stat(directory.c_str(), &details) == 0 && S_ISDIR(details.st_mode));
        gateway.stop();
    }
    close(serial[1]);
    assert(chmod(directory.c_str(), 0500) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    {
        Gateway gateway("/dev/null", 115200, directory, "", "", "");
        GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
        assert(gateway.start(&error)); /* existing readonly directory is valid for startup reads */
        gateway.stop();
    }
    close(serial[1]);
    assert(chmod(directory.c_str(), 0700) == 0);
    assert(rmdir(directory.c_str()) == 0);
    {
        std::ofstream file(directory.c_str());
        assert(file);
        file << "not a directory";
    }
    {
        Gateway gateway("/dev/null", 115200, directory, "", "", "");
        assert(!gateway.start(&error));
        assert(error.find("cannot create runtime directory") != std::string::npos);
        assert(gateway.serialFd() == -1);
    }
    unlink(directory.c_str());
}
}

void runGatewayIntegrationTests() {
    fprintf(stderr, "integration: gateway reset and EZSP version over pipe\n");
    testResetAndVersionOverPipe();
    fprintf(stderr, "integration: gateway reset timeout retry over pipe\n");
    testResetTimeoutRetriesOverPipe();
    fprintf(stderr, "integration: gateway runtime directory boundaries\n");
    testRuntimeDirectoryBoundaries();
}
