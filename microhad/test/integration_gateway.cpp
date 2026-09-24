#include "ash.h"
#include "gateway.h"
#include "gateway_integration_peer.h"

#include <cassert>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
volatile sig_atomic_t serialIoSignalSeen = 0;
void noteSerialIoSignal(int) { serialIoSignalSeen = 1; }

std::vector<unsigned char> readAvailable(int fd) {
    std::vector<unsigned char> bytes;
    unsigned char buffer[256];
    ssize_t count;
    assert(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
    while ((count = read(fd, buffer, sizeof(buffer))) > 0)
        bytes.insert(bytes.end(), buffer, buffer + count);
    return bytes;
}

std::vector<unsigned char> decodeSingleFrame(const std::vector<unsigned char>& wire) {
    AshTransport transport;
    std::vector<unsigned char> frame;
    if (!wire.empty()) transport.feed(&wire[0], wire.size());
    assert(transport.popFrame(&frame));
    assert(!transport.popFrame(&frame));
    return frame;
}

std::vector<std::vector<unsigned char> > decodeFrames(const std::vector<unsigned char>& wire) {
    AshTransport transport;
    std::vector<std::vector<unsigned char> > frames;
    std::vector<unsigned char> frame;
    if (!wire.empty()) transport.feed(&wire[0], wire.size());
    while (transport.popFrame(&frame)) frames.push_back(frame);
    return frames;
}

void sendAshControl(int fd, unsigned char control, const std::vector<unsigned char>& payload) {
    std::vector<unsigned char> wire = AshTransport::encodeData(control, payload);
    assert(write(fd, &wire[0], wire.size()) == static_cast<ssize_t>(wire.size()));
}

std::vector<unsigned char> obfuscateEzsp(const std::vector<unsigned char>& plain) {
    std::vector<unsigned char> encoded;
    unsigned char random = 0x42;
    unsigned long i;
    for (i = 0; i < plain.size(); ++i) {
        encoded.push_back(plain[i] ^ random);
        random = (random & 1) ? static_cast<unsigned char>((random >> 1) ^ 0xb8) :
                                static_cast<unsigned char>(random >> 1);
    }
    return encoded;
}

std::vector<unsigned char> firstAshData(const std::vector<unsigned char>& wire) {
    std::vector<std::vector<unsigned char> > frames = decodeFrames(wire);
    unsigned long i;
    for (i = 0; i < frames.size(); ++i) {
        if (!frames[i].empty() && (frames[i][0] & 0x80) == 0) {
            std::vector<unsigned char> data(frames[i].begin() + 1, frames[i].end());
            return obfuscateEzsp(data);
        }
    }
    assert(false && "missing ASH data frame");
    return std::vector<unsigned char>();
}

void replyToFormationCommand(int fd, const std::vector<unsigned char>& request, unsigned char ashSequence,
                             int acknowledgement = -1, unsigned short responseCommand = 0xffff,
                             unsigned char responseStatus = 0) {
    assert(request.size() >= 5);
    std::vector<unsigned char> reply;
    reply.push_back(request[0]); reply.push_back(0x80); reply.push_back(1);
    unsigned short command = responseCommand == 0xffff ?
        static_cast<unsigned short>(request[3] | (static_cast<unsigned short>(request[4]) << 8)) : responseCommand;
    reply.push_back(static_cast<unsigned char>(command));
    reply.push_back(static_cast<unsigned char>(command >> 8));
    reply.push_back(responseStatus);
    if (command == 0x005a) {
        reply.push_back(0); reply.push_back(0); reply.push_back(0);
    } else if (command == 0x0026) {
        reply[5] = 0x10;
        reply.push_back(0x21); reply.push_back(0x32); reply.push_back(0x43); reply.push_back(0x54);
        reply.push_back(0x65); reply.push_back(0x76); reply.push_back(0x87);
    }
    unsigned char ack = acknowledgement < 0 ? static_cast<unsigned char>((ashSequence + 1) & 7) :
                                              static_cast<unsigned char>(acknowledgement & 7);
    std::vector<unsigned char> wire = AshTransport::encodeData(
        static_cast<unsigned char>(((ashSequence & 7) << 4) | ack), obfuscateEzsp(reply));
    assert(write(fd, &wire[0], wire.size()) == static_cast<ssize_t>(wire.size()));
}

void expectUsageFailure(bool missingConfigPath) {
    int capture[2];
    int status;
    char buffer[512];
    ssize_t count;
    std::string output;
    assert(pipe(capture) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(capture[0]);
        assert(dup2(capture[1], STDERR_FILENO) >= 0);
        close(capture[1]);
        if (missingConfigPath) execl("build/host/microhad-zigbee-native", "microhad-zigbee-native", "--config", static_cast<char*>(0));
        else execl("build/host/microhad-zigbee-native", "microhad-zigbee-native", "--invalid", static_cast<char*>(0));
        _exit(127);
    }
    close(capture[1]);
    while ((count = read(capture[0], buffer, sizeof(buffer))) > 0) output.append(buffer, static_cast<unsigned long>(count));
    close(capture[0]);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 2);
    assert(output.find("usage: microhad-zigbee-native [--config /protected/path/config.json]") != std::string::npos);
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
    std::vector<unsigned char> wire = AshTransport::encodeData(0x01, obfuscateEzsp(versionReply));
    assert(write(serial[1], &wire[0], wire.size()) == static_cast<ssize_t>(wire.size()));
    gateway.poll();
    std::vector<std::vector<unsigned char> > exchange = decodeFrames(readAvailable(serial[1]));
    assert(exchange.size() == 2 && exchange[0][0] == 0x81 && exchange[1][0] == 0x11);
    std::vector<unsigned char> expectedRequest(5, 0);
    expectedRequest[1] = 0x80; expectedRequest[3] = 0; expectedRequest[4] = 0;
    std::vector<unsigned char> actualRequest(exchange[1].begin() + 1, exchange[1].end());
    actualRequest = obfuscateEzsp(actualRequest);
    assert(actualRequest.size() >= expectedRequest.size() && actualRequest[3] == 0 && actualRequest[4] == 0);

    std::vector<unsigned char> negotiatedReply;
    negotiatedReply.push_back(1); negotiatedReply.push_back(0x80); negotiatedReply.push_back(0);
    negotiatedReply.push_back(0); negotiatedReply.push_back(0); negotiatedReply.push_back(8);
    negotiatedReply.push_back(2); negotiatedReply.push_back(0x80); negotiatedReply.push_back(0x67);
    wire = AshTransport::encodeData(0x12, obfuscateEzsp(negotiatedReply));
    assert(write(serial[1], &wire[0], wire.size()) == static_cast<ssize_t>(wire.size()));
    gateway.poll();
    std::vector<std::vector<unsigned char> > initialization = decodeFrames(readAvailable(serial[1]));
    assert(GatewayIntegrationTestPeer::online(gateway));
    assert(gateway.logsJson().find("Negotiated EZSP v8 stack type=2 stack version=0x6780") != std::string::npos);
    assert(initialization.size() == 2 && initialization[0][0] == 0x82 && initialization[1].size() > 6);
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

void runEzspVersionFailure(unsigned int scenario) {
    int serial[2];
    std::string error;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    assert(gateway.start(&error));
    readAvailable(serial[1]);
    const unsigned char resetAck[] = {0x1a, 0xc1, 0x02, 0x0b, 0x0a, 0x52, 0x7e};
    assert(write(serial[1], resetAck, sizeof(resetAck)) == static_cast<ssize_t>(sizeof(resetAck)));
    gateway.poll();
    std::vector<unsigned char> request = readAvailable(serial[1]);
    assert(!request.empty());
    if (scenario == 0 || scenario == 1) {
        std::vector<unsigned char> reply;
        reply.push_back(0); reply.push_back(0x80); reply.push_back(0);
        if (scenario == 0) {
            reply.push_back(6); reply.push_back(2); reply.push_back(0x80); reply.push_back(0x67);
        }
        std::vector<unsigned char> response = AshTransport::encodeData(0x01, obfuscateEzsp(reply));
        assert(write(serial[1], &response[0], response.size()) == static_cast<ssize_t>(response.size()));
        gateway.poll();
        readAvailable(serial[1]);
        assert(!GatewayIntegrationTestPeer::online(gateway));
        std::string logs = gateway.logsJson();
        assert(logs.find(scenario == 0 ? "Unsupported EZSP protocol" : "Truncated EZSP version reply") != std::string::npos);
    } else if (scenario == 2) {
        std::vector<unsigned char> stale;
        stale.push_back(0); stale.push_back(0x80); stale.push_back(0);
        stale.push_back(8); stale.push_back(2); stale.push_back(0x80); stale.push_back(0x67);
        std::vector<unsigned char> response = AshTransport::encodeData(0x11, obfuscateEzsp(stale));
        assert(write(serial[1], &response[0], response.size()) == static_cast<ssize_t>(response.size()));
        gateway.poll();
        std::vector<unsigned char> nak = decodeSingleFrame(readAvailable(serial[1]));
        assert(nak[0] == 0xa0 && !GatewayIntegrationTestPeer::online(gateway));
        assert(gateway.logsJson().find("ASH receive gap; requested retransmission") != std::string::npos);
    } else {
        unsigned int retry;
        for (retry = 0; retry < 3; ++retry) {
            GatewayIntegrationTestPeer::expireAshAck(&gateway);
            gateway.poll();
            assert(!readAvailable(serial[1]).empty());
        }
        GatewayIntegrationTestPeer::expireAshAck(&gateway);
        gateway.poll();
        std::vector<unsigned char> reset = readAvailable(serial[1]);
        assert(reset.size() >= 2 && reset[0] == 0x1a && reset[1] == 0xc0);
        assert(gateway.logsJson().find("ASH acknowledgement failed repeatedly") != std::string::npos);
        assert(!GatewayIntegrationTestPeer::online(gateway));
    }
    gateway.stop(); close(serial[1]);
}

void testEzspVersionFailureModes() {
    runEzspVersionFailure(0); /* unsupported */
    runEzspVersionFailure(1); /* malformed */
    runEzspVersionFailure(2); /* stale ASH sequence */
    runEzspVersionFailure(3); /* absent reply and bounded retry */
}

void testEzspFailedInitializationStatus() {
    int serial[2];
    std::string error;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    assert(gateway.start(&error));
    readAvailable(serial[1]);
    const unsigned char resetAck[] = {0x1a, 0xc1, 0x02, 0x0b, 0x0a, 0x52, 0x7e};
    assert(write(serial[1], resetAck, sizeof(resetAck)) == static_cast<ssize_t>(sizeof(resetAck)));
    gateway.poll(); readAvailable(serial[1]);
    std::vector<unsigned char> supported;
    supported.push_back(0); supported.push_back(0x80); supported.push_back(0);
    supported.push_back(8); supported.push_back(2); supported.push_back(0x80); supported.push_back(0x67);
    std::vector<unsigned char> response = AshTransport::encodeData(0x01, obfuscateEzsp(supported));
    assert(write(serial[1], &response[0], response.size()) == static_cast<ssize_t>(response.size()));
    gateway.poll();
    std::vector<std::vector<unsigned char> > exchange = decodeFrames(readAvailable(serial[1]));
    assert(exchange.size() == 2);
    std::vector<unsigned char> request(exchange[1].begin() + 1, exchange[1].end());
    request = obfuscateEzsp(request);
    assert(request.size() >= 5 && request[3] == 0 && request[4] == 0);
    std::vector<unsigned char> negotiated;
    negotiated.push_back(1); negotiated.push_back(0x80); negotiated.push_back(0);
    negotiated.push_back(0); negotiated.push_back(0); negotiated.push_back(8);
    negotiated.push_back(2); negotiated.push_back(0x80); negotiated.push_back(0x67);
    response = AshTransport::encodeData(0x12, obfuscateEzsp(negotiated));
    assert(write(serial[1], &response[0], response.size()) == static_cast<ssize_t>(response.size()));
    gateway.poll();
    std::vector<std::vector<unsigned char> > initialization = decodeFrames(readAvailable(serial[1]));
    assert(initialization.size() == 2 && GatewayIntegrationTestPeer::online(gateway));
    std::vector<unsigned char> initRequest(initialization[1].begin() + 1, initialization[1].end());
    initRequest = obfuscateEzsp(initRequest);
    assert(initRequest.size() >= 5);
    std::vector<unsigned char> failed;
    failed.push_back(initRequest[0]); failed.push_back(0x80); failed.push_back(1);
    failed.push_back(initRequest[3]); failed.push_back(initRequest[4]); failed.push_back(0x30);
    response = AshTransport::encodeData(0x23, obfuscateEzsp(failed));
    assert(write(serial[1], &response[0], response.size()) == static_cast<ssize_t>(response.size()));
    gateway.poll();
    assert(!GatewayIntegrationTestPeer::online(gateway));
    assert(gateway.logsJson().find("Coordinator initialization command") != std::string::npos);
    gateway.stop(); close(serial[1]);
}

void testFreshNetworkFormationOverSerial() {
    char name[] = "/tmp/microhad-zigbee-network-create-XXXXXX";
    char* temporary = mkdtemp(name);
    int serial[2];
    std::string error;
    assert(temporary != 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    std::string directory = temporary;
    Gateway gateway("/dev/null", 115200, directory, "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    assert(gateway.start(&error));
    readAvailable(serial[1]);
    assert(GatewayIntegrationTestPeer::prepareFreshNetwork(&gateway, &error));

    std::vector<unsigned char> request = firstAshData(readAvailable(serial[1]));
    assert(request[3] == 0xb1 && request[4] == 0x00);
    replyToFormationCommand(serial[1], request, 0);
    gateway.poll();
    request = firstAshData(readAvailable(serial[1]));
    assert(request[3] == 0x68 && request[4] == 0x00);
    assert(request.size() == 48);
    replyToFormationCommand(serial[1], request, 1);
    gateway.poll();
    request = firstAshData(readAvailable(serial[1]));
    assert(request[3] == 0x1e && request[4] == 0x00);
    assert(request.size() == 5 + 8 + 2 + 10);
    replyToFormationCommand(serial[1], request, 2);
    gateway.poll();
    request = firstAshData(readAvailable(serial[1]));
    assert(request[3] == 0x26 && request[4] == 0);
    replyToFormationCommand(serial[1], request, 3, 4);
    gateway.poll();
    request = firstAshData(readAvailable(serial[1]));
    assert(request[3] == 0xab && request[4] == 0);
    assert(request.size() == 8 && request[5] == 7 && request[6] == 1 && request[7] == 1);

    std::string status = gateway.statusJson();
    assert(status.find("\"network_forming\":false") != std::string::npos);
    assert(status.find("\"network_formed\":true") != std::string::npos);
    NetworkState persisted;
    assert(loadNetworkState(directory + "/network.json", &persisted, &error));
    assert(persisted.formed && persisted.channel == 11 && persisted.panId != 0 && persisted.panId != 0xffff);
    assert(persisted.extendedPan.size() == 8 && persisted.networkKey.size() == 16 &&
           persisted.preconfiguredKey.size() == 16);
    assert(gateway.logsJson().find("Leaving the existing Zigbee network") == std::string::npos);
    assert(gateway.logsJson().find("Creating replacement Zigbee network from network-down state") != std::string::npos);
    gateway.stop(); close(serial[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway restored("/dev/null", 115200, directory, "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&restored, serial[0]);
    assert(restored.start(&error));
    readAvailable(serial[1]);
    assert(GatewayIntegrationTestPeer::restoredNetworkMaterial(restored));
    assert(restored.statusJson().find("\"network_formed\":false") != std::string::npos);
    const unsigned char resetAck[] = {0x1a, 0xc1, 0x02, 0x0b, 0x0a, 0x52, 0x7e};
    assert(write(serial[1], resetAck, sizeof(resetAck)) == static_cast<ssize_t>(sizeof(resetAck)));
    restored.poll(); readAvailable(serial[1]);
    std::vector<unsigned char> networkUp;
    networkUp.push_back(0); networkUp.push_back(0x80); networkUp.push_back(0x19); networkUp.push_back(0x90);
    std::vector<unsigned char> wire = AshTransport::encodeData(0x01, obfuscateEzsp(networkUp));
    assert(write(serial[1], &wire[0], wire.size()) == static_cast<ssize_t>(wire.size()));
    restored.poll();
    assert(restored.statusJson().find("\"network_formed\":true") != std::string::npos);
    restored.stop(); close(serial[1]);

    char interruptedName[] = "/tmp/microhad-zigbee-network-interrupted-XXXXXX";
    char* interruptedPath = mkdtemp(interruptedName);
    assert(interruptedPath != 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    std::string interruptedDirectory = interruptedPath;
    Gateway interrupted("/dev/null", 115200, interruptedDirectory, "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&interrupted, serial[0]);
    assert(interrupted.start(&error)); readAvailable(serial[1]);
    assert(GatewayIntegrationTestPeer::prepareFreshNetwork(&interrupted, &error));
    readAvailable(serial[1]);
    assert(interrupted.statusJson().find("\"network_forming\":true") != std::string::npos);
    interrupted.stop(); close(serial[1]);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway afterInterrupted("/dev/null", 115200, interruptedDirectory, "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&afterInterrupted, serial[0]);
    assert(afterInterrupted.start(&error)); readAvailable(serial[1]);
    assert(afterInterrupted.statusJson().find("\"network_forming\":false") != std::string::npos);
    assert(afterInterrupted.statusJson().find("\"network_formed\":false") != std::string::npos);
    assert(loadNetworkState(interruptedDirectory + "/network.json", &persisted, &error) && !persisted.formed);
    afterInterrupted.stop(); close(serial[1]);
    unlink((directory + "/network.json").c_str()); rmdir(directory.c_str());
    rmdir(interruptedDirectory.c_str());
}

void testNetworkCreationFailuresAndTimeout() {
    std::string error;
    Gateway offline("/dev/null", 115200, "/tmp", "", "", "");
    assert(!offline.createFreshNetwork(&error) && error == "Zigbee coordinator is offline");

    Gateway randomFailure("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::setNetworkReady(&randomFailure, false);
    GatewayIntegrationTestPeer::setNetworkRandomSource(&randomFailure, "/tmp/microhad-zigbee-no-random-source");
    assert(!randomFailure.createFreshNetwork(&error) && error == "cannot read system random source");
    assert(!GatewayIntegrationTestPeer::networkForming(randomFailure));

    char name[] = "/tmp/microhad-zigbee-network-failure-XXXXXX";
    char* temporary = mkdtemp(name);
    assert(temporary != 0);
    std::string directory = temporary;
    NetworkState original;
    original.formed = true; original.panId = 0x4567; original.channel = 15;
    original.extendedPan.assign(8, 0x12); original.networkKey.assign(16, 0x34);
    original.preconfiguredKey.assign(16, 0x56);
    assert(saveNetworkState(directory + "/network.json", original, &error));

    int serial[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway rejected("/dev/null", 115200, directory, "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&rejected, serial[0]);
    assert(rejected.start(&error)); readAvailable(serial[1]);
    GatewayIntegrationTestPeer::setNetworkReady(&rejected, false);
    assert(rejected.createFreshNetwork(&error));
    assert(GatewayIntegrationTestPeer::networkForming(rejected));
    assert(!rejected.createFreshNetwork(&error) && error == "network creation is already in progress");
    std::vector<unsigned char> clearKeys = firstAshData(readAvailable(serial[1]));
    assert(clearKeys[3] == 0xb1 && clearKeys[4] == 0);
    replyToFormationCommand(serial[1], clearKeys, 0, -1, 0xffff, 0x30);
    rejected.poll(); readAvailable(serial[1]);
    assert(!GatewayIntegrationTestPeer::networkForming(rejected));
    assert(rejected.logsJson().find("Network formation command 0xb1 rejected") != std::string::npos);
    NetworkState stillSaved;
    assert(loadNetworkState(directory + "/network.json", &stillSaved, &error));
    assert(stillSaved.formed && stillSaved.panId == original.panId && stillSaved.networkKey == original.networkKey);
    rejected.stop(); close(serial[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway timeout("/dev/null", 115200, directory, "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&timeout, serial[0]);
    assert(timeout.start(&error)); readAvailable(serial[1]);
    GatewayIntegrationTestPeer::setNetworkReady(&timeout, false);
    assert(timeout.createFreshNetwork(&error));
    readAvailable(serial[1]);
    sendAshControl(serial[1], 0x81, std::vector<unsigned char>()); /* NCP accepted ASH, no EZSP response */
    timeout.poll(); readAvailable(serial[1]);
    GatewayIntegrationTestPeer::expireNetworkFormation(&timeout);
    timeout.poll();
    assert(!GatewayIntegrationTestPeer::networkForming(timeout));
    assert(timeout.statusJson().find("\"network_formed\":false") != std::string::npos);
    assert(timeout.logsJson().find("Network formation response timed out") != std::string::npos);
    assert(loadNetworkState(directory + "/network.json", &stillSaved, &error));
    assert(stillSaved.formed && stillSaved.panId == original.panId && stillSaved.networkKey == original.networkKey);
    timeout.stop(); close(serial[1]);
    unlink((directory + "/network.json").c_str()); rmdir(directory.c_str());
}

void testSecureJoinPreparationSuccess() {
    int serial[2];
    std::string error;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    assert(GatewayIntegrationTestPeer::prepareJoining(&gateway, 60, &error));
    assert(gateway.statusJson().find("\"joining_preparing\":true") != std::string::npos);
    assert(gateway.statusJson().find("\"joining_until\":0") != std::string::npos);

    std::vector<unsigned char> request = firstAshData(readAvailable(serial[1]));
    assert(request[3] == 0x55 && request[4] == 0 && request[5] == 6 && request[6] == 0x60 && request[7] == 0);
    replyToFormationCommand(serial[1], request, 0);
    gateway.poll();
    request = firstAshData(readAvailable(serial[1]));
    assert(request[3] == 0x55 && request[4] == 0 && request[5] == 5 && request[6] == 0x51 && request[7] == 0);
    replyToFormationCommand(serial[1], request, 1);
    gateway.poll();
    request = firstAshData(readAvailable(serial[1]));
    assert(request[3] == 0xaf && request[4] == 0 && request.size() == 29);
    assert(std::string(request.begin() + 13, request.end()) == "ZigBeeAlliance09");
    replyToFormationCommand(serial[1], request, 2);
    gateway.poll();
    request = firstAshData(readAvailable(serial[1]));
    assert(request[3] == 0x55 && request[4] == 0 && request[5] == 0 && request[6] == 3 && request[7] == 0);
    replyToFormationCommand(serial[1], request, 3);
    gateway.poll();
    request = firstAshData(readAvailable(serial[1]));
    assert(request[3] == 0x22 && request[4] == 0 && request[5] == 60);
    std::string status = gateway.statusJson();
    assert(status.find("\"joining_preparing\":false") != std::string::npos);
    assert(status.find("\"joining_until\":0") == std::string::npos);
    assert(gateway.logsJson().find("Pairing enabled for 60 seconds") != std::string::npos);
    gateway.stop(); close(serial[1]);
}

void runJoinPolicyFailure(unsigned int failedStep) {
    int serial[2];
    std::string error;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    assert(GatewayIntegrationTestPeer::prepareJoining(&gateway, 45, &error));
    unsigned int step;
    for (step = 1; step <= failedStep; ++step) {
        std::vector<unsigned char> request = firstAshData(readAvailable(serial[1]));
        unsigned short expected = step == 3 ? 0x00af : 0x0055;
        assert(request[3] == static_cast<unsigned char>(expected));
        assert(request[4] == static_cast<unsigned char>(expected >> 8));
        replyToFormationCommand(serial[1], request, static_cast<unsigned char>((step - 1) & 7), -1,
                                0xffff, static_cast<unsigned char>(step == failedStep ? 0x30 : 0));
        gateway.poll();
    }
    readAvailable(serial[1]);
    assert(gateway.statusJson().find("\"joining_preparing\":false") != std::string::npos);
    assert(gateway.statusJson().find("\"joining_until\":0") != std::string::npos);
    assert(gateway.logsJson().find("Cannot set Zigbee") != std::string::npos ||
           gateway.logsJson().find("Cannot add Zigbee transient") != std::string::npos);
    assert(gateway.setJoining(30, &error)); /* a failed attempt permits a fresh request */
    gateway.stop(); close(serial[1]);
}

void runJoinPolicyTimeout(unsigned int timedOutStep) {
    int serial[2];
    std::string error;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    assert(GatewayIntegrationTestPeer::prepareJoining(&gateway, 45, &error));
    unsigned int step;
    for (step = 1; step < timedOutStep; ++step) {
        std::vector<unsigned char> request = firstAshData(readAvailable(serial[1]));
        replyToFormationCommand(serial[1], request, static_cast<unsigned char>((step - 1) & 7));
        gateway.poll();
    }
    readAvailable(serial[1]);
    sendAshControl(serial[1], static_cast<unsigned char>(0x80 | (timedOutStep & 7)),
                   std::vector<unsigned char>()); /* transport ACK without policy response */
    gateway.poll(); readAvailable(serial[1]);
    GatewayIntegrationTestPeer::expireJoinPreparation(&gateway);
    gateway.poll();
    assert(gateway.statusJson().find("\"joining_preparing\":false") != std::string::npos);
    assert(gateway.statusJson().find("\"joining_until\":0") != std::string::npos);
    assert(gateway.logsJson().find("Secure pairing preparation timed out") != std::string::npos);
    assert(gateway.setJoining(30, &error));
    gateway.stop(); close(serial[1]);
}

void testSecureJoinPreparationFailuresAndRetries() {
    unsigned int step;
    for (step = 1; step <= 4; ++step) runJoinPolicyFailure(step);
    for (step = 1; step <= 4; ++step) runJoinPolicyTimeout(step);

    int serial[2];
    std::string error;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    assert(GatewayIntegrationTestPeer::prepareJoining(&gateway, 45, &error));
    std::vector<unsigned char> request = firstAshData(readAvailable(serial[1]));
    replyToFormationCommand(serial[1], request, 0, -1, 0x0053); /* unexpected response */
    gateway.poll();
    std::vector<std::vector<unsigned char> > ack = decodeFrames(readAvailable(serial[1]));
    assert(ack.size() == 1 && ack[0][0] == 0x81);
    assert(gateway.statusJson().find("\"joining_preparing\":true") != std::string::npos);
    assert(!gateway.setJoining(60, &error) && error == "secure join preparation is already in progress");
    replyToFormationCommand(serial[1], request, 1);
    gateway.poll();
    std::vector<unsigned char> nextPolicy = firstAshData(readAvailable(serial[1]));
    assert(nextPolicy[3] == 0x55 && nextPolicy[5] == 5);
    gateway.stop(); close(serial[1]);
}

std::vector<unsigned char> activateJoinWindow(Gateway* gateway, int peerFd, unsigned int seconds,
                                             unsigned char firstSequence = 0) {
    std::string error;
    assert(GatewayIntegrationTestPeer::prepareJoining(gateway, seconds, &error));
    unsigned int step;
    std::vector<unsigned char> request;
    for (step = 0; step < 4; ++step) {
        request = firstAshData(readAvailable(peerFd));
        replyToFormationCommand(peerFd, request, static_cast<unsigned char>((firstSequence + step) & 7));
        gateway->poll();
    }
    return firstAshData(readAvailable(peerFd));
}

long joiningUntilFromStatus(Gateway* gateway) {
    std::string status = gateway->statusJson();
    std::string::size_type field = status.find("\"joining_until\":");
    assert(field != std::string::npos);
    return static_cast<long>(strtol(status.c_str() + field + 16, 0, 10));
}

std::vector<unsigned char> apsPayload(unsigned short profile, unsigned short cluster,
                                      unsigned char sourceEndpoint, unsigned char lqi,
                                      const std::vector<unsigned char>& zcl, unsigned char declaredLength = 0xff);
void sendApsCallback(int fd, unsigned char ashSequence, const std::vector<unsigned char>& aps);
void sendApsCallbackV8(int fd, unsigned char ashSequence, const std::vector<unsigned char>& aps,
                       int acknowledgement = -1);
std::vector<unsigned char> answerInterviewRequest(Gateway* gateway, int peerFd,
                                                   const std::vector<unsigned char>& request,
                                                   unsigned short profile, unsigned short cluster,
                                                   unsigned char endpoint,
                                                   const std::vector<unsigned char>& response);

void testJoinWindowExtensionExpiryRenewalAndCloseFailure() {
    int serial[2];
    std::string error;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);

    std::vector<unsigned char> permit = activateJoinWindow(&gateway, serial[1], 600);
    assert(permit[3] == 0x22 && permit[5] == 254);
    long firstDeadline = joiningUntilFromStatus(&gateway);
    assert(firstDeadline > time(0) + 590);
    replyToFormationCommand(serial[1], permit, 4);
    gateway.poll(); readAvailable(serial[1]);

    assert(gateway.setJoining(120, &error));
    unsigned int step;
    for (step = 0; step < 4; ++step) {
        std::vector<unsigned char> request = firstAshData(readAvailable(serial[1]));
        replyToFormationCommand(serial[1], request, static_cast<unsigned char>(5 + step));
        gateway.poll();
    }
    permit = firstAshData(readAvailable(serial[1]));
    assert(permit[3] == 0x22 && permit[5] == 120);
    long extendedDeadline = joiningUntilFromStatus(&gateway);
    assert(extendedDeadline > firstDeadline - 490 && extendedDeadline < firstDeadline);
    replyToFormationCommand(serial[1], permit, 1);
    gateway.poll(); readAvailable(serial[1]);

    GatewayIntegrationTestPeer::setJoinDeadlines(&gateway, time(0) + 500, time(0) - 1);
    gateway.poll();
    permit = firstAshData(readAvailable(serial[1]));
    assert(permit[3] == 0x22 && permit[5] == 254);
    replyToFormationCommand(serial[1], permit, 2);
    gateway.poll(); readAvailable(serial[1]);

    GatewayIntegrationTestPeer::setJoinDeadlines(&gateway, time(0) - 1, 0);
    gateway.poll();
    permit = firstAshData(readAvailable(serial[1]));
    assert(permit[3] == 0x22 && permit[5] == 0);
    assert(gateway.statusJson().find("\"joining_until\":0") != std::string::npos);
    replyToFormationCommand(serial[1], permit, 3);
    gateway.poll(); readAvailable(serial[1]);

    permit = activateJoinWindow(&gateway, serial[1], 120, 4);
    assert(permit[3] == 0x22 && permit[5] == 120);
    replyToFormationCommand(serial[1], permit, 0);
    gateway.poll(); readAvailable(serial[1]);
    assert(gateway.setJoining(0, &error));
    permit = firstAshData(readAvailable(serial[1]));
    assert(permit[3] == 0x22 && permit[5] == 0);
    std::string beforeFailure = gateway.statusJson();
    assert(beforeFailure.find("\"joining_until\":0") == std::string::npos);
    replyToFormationCommand(serial[1], permit, 1, -1, 0xffff, 0x30);
    gateway.poll(); readAvailable(serial[1]);
    assert(gateway.statusJson().find("\"joining_until\":0") == std::string::npos);
    assert(gateway.logsJson().find("Coordinator rejected closing pairing permit") != std::string::npos);

    assert(gateway.setJoining(0, &error));
    permit = firstAshData(readAvailable(serial[1]));
    replyToFormationCommand(serial[1], permit, 2);
    gateway.poll();
    assert(gateway.statusJson().find("\"joining_until\":0") != std::string::npos);
    gateway.stop(); close(serial[1]);
}

void testIasEnrollmentAndZoneIdExhaustion() {
    char name[] = "/tmp/microhad-zigbee-ias-enroll-XXXXXX";
    char* temporary = mkdtemp(name);
    assert(temporary != 0);
    std::string directory(temporary);
    int serial[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, directory, "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    const std::string targetIeee = "0x00124b0000007001";
    GatewayIntegrationTestPeer::addIasDevice(&gateway, 0x1234, targetIeee, -1);

    std::vector<unsigned char> enroll;
    enroll.push_back(0x19); enroll.push_back(0x44); enroll.push_back(0x01);
    enroll.push_back(0x2a); enroll.push_back(0x00); /* zone type */
    enroll.push_back(0x34); enroll.push_back(0x12); /* manufacturer code */
    sendApsCallbackV8(serial[1], 0, apsPayload(0x0104, 0x0500, 1, 91, enroll));
    gateway.poll();
    std::vector<unsigned char> enrollmentWire = readAvailable(serial[1]);
    std::vector<unsigned char> response = firstAshData(enrollmentWire);
    assert(response[3] == 0x34 && response[4] == 0x00);
    assert(response[5] == 0 && response[6] == 0x34 && response[7] == 0x12);
    assert(response[8] == 0x04 && response[9] == 0x01);
    assert(response[10] == 0 && response[11] == 0x05);
    assert(response[12] == 1 && response[13] == 1 && response[20] == 5);
    assert(response[21] == 0x19 && response[22] == 0x44 && response[23] == 0x00 &&
           response[24] == 0x00 && response[25] == 0x00);

    replyToFormationCommand(serial[1], response, 0);
    gateway.poll(); readAvailable(serial[1]);
    sendApsCallbackV8(serial[1], 1, apsPayload(0x0104, 0x0500, 1, 92, enroll));
    gateway.poll();
    response = firstAshData(readAvailable(serial[1]));
    assert(response[25] == 0); /* repeat enrollment retains the allocated ID */
    replyToFormationCommand(serial[1], response, 1);
    gateway.poll(); readAvailable(serial[1]);

    std::vector<Device> persisted;
    std::string error;
    assert(loadDevices(directory + "/devices.json", &persisted, &error));
    assert(persisted.size() == 1 && persisted[0].ieeeAddress == targetIeee && persisted[0].iasZoneId == 0);
    gateway.stop(); close(serial[1]);
    unlink((directory + "/devices.json").c_str());
    rmdir(directory.c_str());

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway exhausted("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&exhausted, serial[0]);
    unsigned int id;
    char ieee[32];
    for (id = 0; id <= 254; ++id) {
        snprintf(ieee, sizeof(ieee), "0x00124b000000%04x", id);
        GatewayIntegrationTestPeer::addIasDevice(&exhausted, static_cast<unsigned short>(id + 1), ieee,
                                                  static_cast<int>(id));
    }
    GatewayIntegrationTestPeer::addIasDevice(&exhausted, 0x7fff, "0x00124b0000007fff", -1);
    std::vector<unsigned char> exhaustedEnroll = apsPayload(0x0104, 0x0500, 1, 80, enroll);
    exhaustedEnroll[14] = 0xff; exhaustedEnroll[15] = 0x7f;
    sendApsCallbackV8(serial[1], 0, exhaustedEnroll);
    exhausted.poll();
    std::vector<std::vector<unsigned char> > outbound = decodeFrames(readAvailable(serial[1]));
    unsigned long frame;
    for (frame = 0; frame < outbound.size(); ++frame)
        assert(outbound[frame].empty() || (outbound[frame][0] & 0x80) != 0);
    assert(exhausted.logsJson().find("IAS zone ID pool exhausted") != std::string::npos);
    std::vector<Device> exhaustedDevices = exhausted.devices();
    bool targetUnassigned = false;
    for (frame = 0; frame < exhaustedDevices.size(); ++frame) {
        if (exhaustedDevices[frame].ieeeAddress == "0x00124b0000007fff")
            targetUnassigned = exhaustedDevices[frame].iasZoneId == -1;
    }
    assert(targetUnassigned);
    exhausted.stop(); close(serial[1]);
}

std::vector<unsigned char> deviceAnnounceMessage(unsigned short node, const unsigned char* eui) {
    std::vector<unsigned char> message;
    message.push_back(0x21); /* ZDO transaction */
    message.push_back(static_cast<unsigned char>(node));
    message.push_back(static_cast<unsigned char>(node >> 8));
    message.insert(message.end(), eui, eui + 8); /* NCP byte order: least significant first */
    message.push_back(0x8e); /* receiver-on-when-idle, router, secure join */
    return message;
}

void testDeviceAnnounceRejoinAndNodeReuse() {
    char name[] = "/tmp/microhad-zigbee-device-announce-XXXXXX";
    char* temporary = mkdtemp(name);
    assert(temporary != 0);
    std::string directory(temporary);
    int serial[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, directory, "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    GatewayIntegrationTestPeer::setNetworkReady(&gateway, true);
    const unsigned char euiA[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const unsigned char euiB[] = {8, 7, 6, 5, 4, 3, 2, 1};

    std::vector<unsigned char> announce = deviceAnnounceMessage(0x1234, euiA);
    sendApsCallbackV8(serial[1], 0, apsPayload(0x0000, 0x0013, 0, 90, announce), 1);
    gateway.poll();
    std::vector<unsigned char> announceWire = readAvailable(serial[1]);
    std::vector<unsigned char> interview = firstAshData(announceWire);
    assert(interview[3] == 0x34 && interview[4] == 0);
    assert(interview[6] == 0x34 && interview[7] == 0x12);
    std::vector<Device> devices = gateway.devices();
    assert(devices.size() == 1 && devices[0].ieeeAddress == "0x0807060504030201");
    assert(devices[0].networkAddress == 0x1234 && devices[0].lastSeen != 0);

    announce = deviceAnnounceMessage(0x1235, euiA);
    sendApsCallbackV8(serial[1], 1, apsPayload(0x0000, 0x0013, 0, 91, announce), 1);
    gateway.poll();
    announceWire = readAvailable(serial[1]);
    interview = firstAshData(announceWire);
    assert(interview[3] == 0x34 && interview[6] == 0x35 && interview[7] == 0x12);
    devices = gateway.devices();
    assert(devices.size() == 1 && devices[0].networkAddress == 0x1235);

    announce = deviceAnnounceMessage(0x1235, euiB);
    sendApsCallbackV8(serial[1], 2, apsPayload(0x0000, 0x0013, 0, 92, announce), 2);
    gateway.poll();
    interview = firstAshData(readAvailable(serial[1]));
    assert(interview[3] == 0x34 && interview[6] == 0x35 && interview[7] == 0x12);
    devices = gateway.devices();
    assert(devices.size() == 2);
    bool stalePreserved = false, replacementMapped = false;
    unsigned long i;
    for (i = 0; i < devices.size(); ++i) {
        if (devices[i].ieeeAddress == "0x0807060504030201") stalePreserved = devices[i].networkAddress == 0xffff;
        if (devices[i].ieeeAddress == "0x0102030405060708") replacementMapped = devices[i].networkAddress == 0x1235;
    }
    assert(stalePreserved && replacementMapped);

    announce = deviceAnnounceMessage(0x1235, euiB);
    sendApsCallbackV8(serial[1], 3, apsPayload(0x0000, 0x0013, 0, 93, announce), 3);
    gateway.poll();
    std::vector<std::vector<unsigned char> > duplicateOutput = decodeFrames(readAvailable(serial[1]));
    for (i = 0; i < duplicateOutput.size(); ++i)
        assert(duplicateOutput[i].empty() || (duplicateOutput[i][0] & 0x80) != 0);
    assert(gateway.logsJson().find("Ignoring duplicate device announcement during interview") != std::string::npos);

    std::vector<Device> persisted;
    std::string error;
    assert(loadDevices(directory + "/devices.json", &persisted, &error));
    assert(persisted.size() == 2);
    stalePreserved = false; replacementMapped = false;
    for (i = 0; i < persisted.size(); ++i) {
        if (persisted[i].ieeeAddress == "0x0807060504030201") stalePreserved = persisted[i].networkAddress == 0xffff;
        if (persisted[i].ieeeAddress == "0x0102030405060708") replacementMapped = persisted[i].networkAddress == 0x1235;
    }
    assert(stalePreserved && replacementMapped);
    gateway.stop(); close(serial[1]);
    unlink((directory + "/devices.json").c_str());
    rmdir(directory.c_str());
}

void testCompleteSensorInterview() {
    char name[] = "/tmp/microhad-zigbee-interview-XXXXXX";
    char* temporary = mkdtemp(name);
    assert(temporary != 0);
    std::string directory(temporary);
    int serial[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, directory, "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    GatewayIntegrationTestPeer::setNetworkReady(&gateway, true);
    GatewayIntegrationTestPeer::setCoordinatorIeeeAddress(&gateway);
    const unsigned char eui[] = {1, 2, 3, 4, 5, 6, 7, 8};

    std::vector<unsigned char> announce = deviceAnnounceMessage(0x1234, eui);
    sendApsCallbackV8(serial[1], 0, apsPayload(0x0000, 0x0013, 0, 80, announce), 1);
    gateway.poll();
    std::vector<unsigned char> request = firstAshData(readAvailable(serial[1]));
    assert(request[3] == 0x34 && request[4] == 0 && request[6] == 0x34 && request[7] == 0x12);

    std::vector<unsigned char> response;
    response.push_back(request[21]); response.push_back(0); /* Node Descriptor response */
    std::vector<unsigned char> wire = answerInterviewRequest(&gateway, serial[1], request, 0x0000, 0x8002, 0, response);
    request = firstAshData(wire);
    assert(request[10] == 0x05 && request[11] == 0x00); /* Active Endpoints */

    response.clear(); response.push_back(request[21]); response.push_back(0);
    response.push_back(0x34); response.push_back(0x12); response.push_back(1); response.push_back(1);
    wire = answerInterviewRequest(&gateway, serial[1], request, 0x0000, 0x8005, 0, response);
    request = firstAshData(wire);
    assert(request[10] == 0x04 && request[11] == 0x00); /* Simple Descriptor */

    response.clear();
    response.push_back(request[21]); response.push_back(0); response.push_back(0x34); response.push_back(0x12);
    response.push_back(12); /* descriptor bytes */
    response.push_back(1); response.push_back(0x04); response.push_back(0x01);
    response.push_back(0x01); response.push_back(0x00); response.push_back(0);
    response.push_back(2); response.push_back(0x00); response.push_back(0x00);
    response.push_back(0x02); response.push_back(0x04); response.push_back(0);
    wire = answerInterviewRequest(&gateway, serial[1], request, 0x0000, 0x8004, 0, response);
    request = firstAshData(wire);
    assert(request[10] == 0x00 && request[11] == 0x00 && request[23] == 0x00);
    assert(request[24] == 0x04 && request[26] == 0x05); /* Basic attributes 0004/0005/4000 */

    response.clear();
    response.push_back(0x18); response.push_back(request[22]); response.push_back(0x01);
    response.push_back(0x04); response.push_back(0); response.push_back(0); response.push_back(0x42); response.push_back(4);
    response.insert(response.end(), "Acme", "Acme" + 4);
    response.push_back(0x05); response.push_back(0); response.push_back(0); response.push_back(0x42); response.push_back(6);
    response.insert(response.end(), "ModelX", "ModelX" + 6);
    response.push_back(0x00); response.push_back(0x40); response.push_back(0); response.push_back(0x42); response.push_back(3);
    response.insert(response.end(), "1.0", "1.0" + 3);
    wire = answerInterviewRequest(&gateway, serial[1], request, 0x0104, 0x0000, 1, response);
    request = firstAshData(wire);
    assert(request[10] == 0x21 && request[11] == 0x00); /* Bind temperature reports */
    assert(request[21] != 0 && request[22] == 1 && request[23] == 2 &&
           request[24] == 3 && request[25] == 4 && request[30] == 1);

    response.clear(); response.push_back(request[21]); response.push_back(0);
    wire = answerInterviewRequest(&gateway, serial[1], request, 0x0000, 0x8021, 0, response);
    request = firstAshData(wire);
    assert(request[10] == 0x02 && request[11] == 0x04 && request[23] == 0x06);
    assert(request[27] == 0x29 && request[28] == 30 && request[29] == 0 &&
           request[30] == 0x10 && request[31] == 0x0e);

    response.clear(); response.push_back(0x18); response.push_back(request[22]);
    response.push_back(0x07); response.push_back(0);
    wire = answerInterviewRequest(&gateway, serial[1], request, 0x0104, 0x0402, 1, response);
    request = firstAshData(wire);
    assert(request[10] == 0x02 && request[11] == 0x04 && request[23] == 0x00);

    response.clear(); response.push_back(0x18); response.push_back(request[22]); response.push_back(0x01);
    response.push_back(0); response.push_back(0); response.push_back(0); response.push_back(0x29);
    response.push_back(0x2c); response.push_back(0x09);
    wire = answerInterviewRequest(&gateway, serial[1], request, 0x0104, 0x0402, 1, response);
    std::vector<std::vector<unsigned char> > finishedFrames = decodeFrames(wire);
    unsigned long i;
    for (i = 0; i < finishedFrames.size(); ++i)
        assert(finishedFrames[i].empty() || (finishedFrames[i][0] & 0x80) != 0);

    std::vector<Device> devices = gateway.devices();
    assert(devices.size() == 1 && devices[0].manufacturerName == "Acme" && devices[0].modelId == "ModelX");
    assert(devices[0].hasTemperature && devices[0].temperatureCelsius > 23.4 && devices[0].temperatureCelsius < 23.6);
    std::vector<Device> persisted;
    std::string error;
    assert(loadDevices(directory + "/devices.json", &persisted, &error));
    assert(persisted.size() == 1 && persisted[0].endpoints.size() == 1);
    assert(persisted[0].manufacturerName == "Acme" && persisted[0].modelId == "ModelX");
    assert(gateway.logsJson().find("Sensor interview completed") != std::string::npos);
    gateway.stop(); close(serial[1]);
    unlink((directory + "/devices.json").c_str());
    rmdir(directory.c_str());
}

void testInterviewResponseMatchingAndTimeout() {
    int serial[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    const std::string ieee = "0x00124b0000001234";
    GatewayIntegrationTestPeer::addIasDevice(&gateway, 0x1234, ieee, -1);
    GatewayIntegrationTestPeer::startDeviceInterview(&gateway, 0x1234);
    gateway.poll();
    std::vector<unsigned char> request = firstAshData(readAvailable(serial[1]));
    assert(request[10] == 0x02 && request[11] == 0x00);

    unsigned char receiveSequence = GatewayIntegrationTestPeer::ashReceiveSequence(gateway);
    unsigned char acknowledgement = static_cast<unsigned char>(
        (GatewayIntegrationTestPeer::ashSendSequence(gateway) + 1) & 7);
    replyToFormationCommand(serial[1], request, receiveSequence, acknowledgement);
    gateway.poll(); readAvailable(serial[1]);

    std::vector<unsigned char> zdo;
    zdo.push_back(static_cast<unsigned char>(request[21] + 1)); zdo.push_back(0);
    receiveSequence = GatewayIntegrationTestPeer::ashReceiveSequence(gateway);
    acknowledgement = static_cast<unsigned char>((GatewayIntegrationTestPeer::ashSendSequence(gateway) + 1) & 7);
    sendApsCallbackV8(serial[1], receiveSequence, apsPayload(0x0000, 0x8002, 0, 70, zdo), acknowledgement);
    gateway.poll();
    std::vector<unsigned char> wire = readAvailable(serial[1]);
    std::vector<std::vector<unsigned char> > frames = decodeFrames(wire);
    unsigned long i;
    for (i = 0; i < frames.size(); ++i) assert(frames[i].empty() || (frames[i][0] & 0x80) != 0);
    assert(gateway.logsJson().find("Node descriptor received") == std::string::npos);

    std::vector<unsigned char> delivery(16, 0);
    delivery[14] = static_cast<unsigned char>(request[19] + 1); /* unrelated message tag */
    delivery[15] = 0x01; /* failure for another transmission */
    std::vector<unsigned char> ezsp;
    ezsp.push_back(1); ezsp.push_back(0x80); ezsp.push_back(16); ezsp.push_back(0x3f); ezsp.push_back(0);
    ezsp.insert(ezsp.end(), delivery.begin(), delivery.end());
    receiveSequence = GatewayIntegrationTestPeer::ashReceiveSequence(gateway);
    sendAshControl(serial[1], static_cast<unsigned char>((receiveSequence << 4) |
        ((GatewayIntegrationTestPeer::ashSendSequence(gateway) + 1) & 7)), obfuscateEzsp(ezsp));
    gateway.poll();
    wire = readAvailable(serial[1]); frames = decodeFrames(wire);
    for (i = 0; i < frames.size(); ++i) assert(frames[i].empty() || (frames[i][0] & 0x80) != 0);

    zdo[0] = request[21]; /* delayed, correctly matched node descriptor */
    receiveSequence = GatewayIntegrationTestPeer::ashReceiveSequence(gateway);
    sendApsCallbackV8(serial[1], receiveSequence, apsPayload(0x0000, 0x8002, 0, 70, zdo),
                      (GatewayIntegrationTestPeer::ashSendSequence(gateway) + 1) & 7);
    gateway.poll();
    std::vector<unsigned char> activeEndpoints = firstAshData(readAvailable(serial[1]));
    assert(activeEndpoints[10] == 0x05 && activeEndpoints[11] == 0x00);

    zdo[0] = request[21]; /* duplicate old response cannot complete active-endpoint operation */
    receiveSequence = GatewayIntegrationTestPeer::ashReceiveSequence(gateway);
    sendApsCallbackV8(serial[1], receiveSequence, apsPayload(0x0000, 0x8002, 0, 70, zdo),
                      (GatewayIntegrationTestPeer::ashSendSequence(gateway) + 1) & 7);
    gateway.poll();
    wire = readAvailable(serial[1]); frames = decodeFrames(wire);
    for (i = 0; i < frames.size(); ++i) assert(frames[i].empty() || (frames[i][0] & 0x80) != 0);

    std::vector<unsigned char> malformed(1, activeEndpoints[21]);
    receiveSequence = GatewayIntegrationTestPeer::ashReceiveSequence(gateway);
    sendApsCallbackV8(serial[1], receiveSequence, apsPayload(0x0000, 0x8005, 0, 70, malformed),
                      (GatewayIntegrationTestPeer::ashSendSequence(gateway) + 1) & 7);
    gateway.poll();
    wire = readAvailable(serial[1]); frames = decodeFrames(wire);
    for (i = 0; i < frames.size(); ++i) assert(frames[i].empty() || (frames[i][0] & 0x80) != 0);

    std::vector<unsigned char> failed(2, 0);
    failed[0] = activeEndpoints[21]; failed[1] = 0x80;
    receiveSequence = GatewayIntegrationTestPeer::ashReceiveSequence(gateway);
    sendApsCallbackV8(serial[1], receiveSequence, apsPayload(0x0000, 0x8005, 0, 70, failed),
                      (GatewayIntegrationTestPeer::ashSendSequence(gateway) + 1) & 7);
    gateway.poll();
    wire = readAvailable(serial[1]); frames = decodeFrames(wire);
    for (i = 0; i < frames.size(); ++i) assert(frames[i].empty() || (frames[i][0] & 0x80) != 0);
    assert(gateway.logsJson().find("Active endpoints received") == std::string::npos);

    GatewayIntegrationTestPeer::expireInterview(&gateway);
    gateway.poll();
    assert(gateway.logsJson().find("Interview operation failed for " + ieee + ": response timed out") != std::string::npos);
    assert(gateway.logsJson().find("Interview paused until the device wakes again") != std::string::npos);
    gateway.stop(); close(serial[1]);
}

void testCoordinatorInitializationSequence() {
    int serial[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    GatewayIntegrationTestPeer::beginCoordinatorInitialization(&gateway);
    unsigned int step;
    for (step = 0; step < 22; ++step) {
        std::vector<unsigned char> wire = readAvailable(serial[1]);
        std::vector<unsigned char> request = firstAshData(wire);
        unsigned short command = static_cast<unsigned short>(request[3]) |
                                 (static_cast<unsigned short>(request[4]) << 8);
        unsigned short expected;
        if (step < 12) expected = 0x0053;
        else if (step < 15) expected = 0x0055;
        else if (step < 17) expected = 0x00ab;
        else if (step == 17) expected = 0x0010;
        else if (step == 18) expected = 0x005a;
        else if (step < 21) expected = 0x0002;
        else expected = 0x0017;
        assert(command == expected);
        if (step == 0) assert(request.size() == 8 && request[5] == 56 && request[6] == 90 && request[7] == 0);
        if (step == 12) assert(request.size() == 8 && request[5] == 6 && request[6] == 0x60 && request[7] == 0);
        if (step == 15) assert(request.size() == 8 && request[5] == 63 && request[6] == 1 && request[7] == 3);
        replyToFormationCommand(serial[1], request, static_cast<unsigned char>(step & 7));
        gateway.poll();
    }
    std::string logs = gateway.logsJson();
    assert(logs.find("Initializing NCP Zigbee network after coordinator configuration") != std::string::npos);
    assert(logs.find("NCP network initialization accepted") != std::string::npos);

    std::vector<unsigned char> networkUp;
    networkUp.push_back(22); networkUp.push_back(0x80); networkUp.push_back(0);
    networkUp.push_back(0x19); networkUp.push_back(0); networkUp.push_back(0x90);
    sendAshControl(serial[1], 0x67, obfuscateEzsp(networkUp));
    gateway.poll();
    std::vector<unsigned char> networkUpWire = readAvailable(serial[1]);
    std::vector<unsigned char> ieeeRequest = firstAshData(networkUpWire);
    assert(ieeeRequest[3] == 0x26 && ieeeRequest[4] == 0);
    replyToFormationCommand(serial[1], ieeeRequest, 7, 7);
    gateway.poll();
    std::vector<unsigned char> bindWire = readAvailable(serial[1]);
    std::vector<unsigned char> bindGroupRequest = firstAshData(bindWire);
    assert(bindGroupRequest[3] == 0x64 && bindGroupRequest[4] == 0);
    assert(bindGroupRequest.size() == 10 && bindGroupRequest[5] == 0 &&
           bindGroupRequest[6] == 0x85 && bindGroupRequest[7] == 3 && bindGroupRequest[8] == 1);
    assert(gateway.logsJson().find("Coordinator IEEE address: 0x8776655443322110") != std::string::npos);
    assert(gateway.logsJson().find("Subscribing coordinator to IKEA legacy remote group 901") != std::string::npos);
    gateway.stop(); close(serial[1]);
}

void testCoordinatorInitializationUnexpectedReplyAndTimeout() {
    int serial[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    GatewayIntegrationTestPeer::beginCoordinatorInitialization(&gateway);
    std::vector<unsigned char> request = firstAshData(readAvailable(serial[1]));
    replyToFormationCommand(serial[1], request, 0, -1, 0x0055);
    gateway.poll();
    std::vector<std::vector<unsigned char> > unexpected = decodeFrames(readAvailable(serial[1]));
    assert(unexpected.size() == 1 && unexpected[0][0] == 0x81);
    assert(GatewayIntegrationTestPeer::coordinatorInitializationStep(gateway) == 1);
    assert(gateway.logsJson().find("EZSP <- v8 0x55") != std::string::npos);
    replyToFormationCommand(serial[1], request, 1);
    gateway.poll();
    std::vector<unsigned char> delayed = firstAshData(readAvailable(serial[1]));
    assert(delayed[3] == 0x53 && delayed[5] == 25);
    assert(GatewayIntegrationTestPeer::coordinatorInitializationStep(gateway) == 2);
    gateway.stop(); close(serial[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway timeout("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&timeout, serial[0]);
    GatewayIntegrationTestPeer::beginCoordinatorInitialization(&timeout);
    readAvailable(serial[1]);
    sendAshControl(serial[1], 0x81, std::vector<unsigned char>()); /* transport ACK, no EZSP reply */
    timeout.poll(); readAvailable(serial[1]);
    GatewayIntegrationTestPeer::expireCoordinatorInitialization(&timeout);
    timeout.poll();
    std::vector<unsigned char> reset = readAvailable(serial[1]);
    assert(reset.size() >= 2 && reset[0] == 0x1a && reset[1] == 0xc0);
    assert(!GatewayIntegrationTestPeer::online(timeout));
    assert(GatewayIntegrationTestPeer::coordinatorInitializationStep(timeout) == 0);
    assert(timeout.logsJson().find("Coordinator initialization response timed out") != std::string::npos);
    timeout.stop(); close(serial[1]);
}

std::vector<unsigned char> apsPayload(unsigned short profile, unsigned short cluster,
                                      unsigned char sourceEndpoint, unsigned char lqi,
                                      const std::vector<unsigned char>& zcl, unsigned char declaredLength) {
    std::vector<unsigned char> payload(19 + zcl.size(), 0);
    payload[0] = 0;
    payload[1] = static_cast<unsigned char>(profile); payload[2] = static_cast<unsigned char>(profile >> 8);
    payload[3] = static_cast<unsigned char>(cluster); payload[4] = static_cast<unsigned char>(cluster >> 8);
    payload[5] = sourceEndpoint; payload[6] = 1;
    payload[7] = 0x40; payload[11] = 42; payload[12] = lqi; payload[13] = static_cast<unsigned char>(-40);
    payload[14] = 0x34; payload[15] = 0x12;
    payload[18] = declaredLength == 0xff ? static_cast<unsigned char>(zcl.size()) : declaredLength;
    std::copy(zcl.begin(), zcl.end(), payload.begin() + 19);
    return payload;
}

void sendApsCallback(int fd, unsigned char ashSequence, const std::vector<unsigned char>& aps) {
    std::vector<unsigned char> ezsp;
    ezsp.push_back(1); ezsp.push_back(0x80); ezsp.push_back(0x45);
    ezsp.insert(ezsp.end(), aps.begin(), aps.end());
    sendAshControl(fd, static_cast<unsigned char>((ashSequence & 7) << 4), obfuscateEzsp(ezsp));
}

void sendApsCallbackV8(int fd, unsigned char ashSequence, const std::vector<unsigned char>& aps,
                       int acknowledgement) {
    std::vector<unsigned char> ezsp;
    ezsp.push_back(1); ezsp.push_back(0x80); ezsp.push_back(static_cast<unsigned char>(aps.size()));
    ezsp.push_back(0x45); ezsp.push_back(0);
    ezsp.insert(ezsp.end(), aps.begin(), aps.end());
    unsigned char ack = acknowledgement < 0 ? static_cast<unsigned char>((ashSequence + 1) & 7) :
                                               static_cast<unsigned char>(acknowledgement & 7);
    sendAshControl(fd, static_cast<unsigned char>(((ashSequence & 7) << 4) | ack),
                   obfuscateEzsp(ezsp));
}

std::vector<unsigned char> answerInterviewRequest(Gateway* gateway, int peerFd,
                                                   const std::vector<unsigned char>& request,
                                                   unsigned short profile, unsigned short cluster,
                                                   unsigned char endpoint,
                                                   const std::vector<unsigned char>& response) {
    unsigned char receiveSequence = GatewayIntegrationTestPeer::ashReceiveSequence(*gateway);
    unsigned char acknowledgement = static_cast<unsigned char>(
        (GatewayIntegrationTestPeer::ashSendSequence(*gateway) + 1) & 7);
    replyToFormationCommand(peerFd, request, receiveSequence, acknowledgement);
    gateway->poll();
    readAvailable(peerFd); /* transport ACK for the command response */
    receiveSequence = GatewayIntegrationTestPeer::ashReceiveSequence(*gateway);
    acknowledgement = static_cast<unsigned char>((GatewayIntegrationTestPeer::ashSendSequence(*gateway) + 1) & 7);
    sendApsCallbackV8(peerFd, receiveSequence,
                      apsPayload(profile, cluster, endpoint, 88, response), acknowledgement);
    gateway->poll();
    return readAvailable(peerFd);
}

void testApsCallbackValidationAndDispatch() {
    int serial[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    GatewayIntegrationTestPeer::prepareApsSensor(&gateway);
    std::vector<unsigned char> report;
    report.push_back(0x18); report.push_back(7); report.push_back(0x0a);
    report.push_back(0); report.push_back(0); report.push_back(0x29); report.push_back(0x2c); report.push_back(0x09);
    sendApsCallback(serial[1], 0, apsPayload(0x0104, 0x0402, 1, 88, report));
    gateway.poll(); readAvailable(serial[1]);
    std::string status = gateway.statusJson();
    assert(status.find("\"temperature_celsius\":23.48") != std::string::npos);
    assert(status.find("\"link_quality\":88") != std::string::npos);
    assert(gateway.logsJson().find("profile=0x104 cluster=0x402 length=8") != std::string::npos);
    assert(gateway.logsJson().find("lqi=88 rssi=-40") != std::string::npos);
    assert(gateway.logsJson().find("aps_seq=42 options=0x40") != std::string::npos);

    sendApsCallback(serial[1], 1, apsPayload(0x0005, 0x0402, 1, 12, report));
    gateway.poll(); readAvailable(serial[1]);
    status = gateway.statusJson();
    assert(status.find("\"temperature_celsius\":23.48") != std::string::npos);
    assert(status.find("\"link_quality\":88") != std::string::npos);
    assert(gateway.logsJson().find("Ignoring APS message with unsupported profile") != std::string::npos);

    sendApsCallback(serial[1], 2, apsPayload(0x0104, 0x0402, 2, 4, report));
    gateway.poll(); readAvailable(serial[1]);
    assert(gateway.statusJson().find("\"link_quality\":88") != std::string::npos);
    assert(gateway.logsJson().find("Ignoring APS message from an undiscovered endpoint") != std::string::npos);

    sendApsCallback(serial[1], 3, apsPayload(0x0104, 0x0402, 1, 20, report, 20));
    gateway.poll(); readAvailable(serial[1]);
    assert(gateway.statusJson().find("\"temperature_celsius\":23.48") != std::string::npos);

    std::vector<unsigned char> unsupported;
    unsupported.push_back(0x18); unsupported.push_back(9); unsupported.push_back(0x0a);
    sendApsCallback(serial[1], 4, apsPayload(0x0104, 0x1234, 1, 77, unsupported));
    gateway.poll(); readAvailable(serial[1]);
    assert(gateway.statusJson().find("\"temperature_celsius\":23.48") != std::string::npos);
    assert(gateway.logsJson().find("profile=0x104 cluster=0x1234") != std::string::npos);
    gateway.stop(); close(serial[1]);
}

void testSerialBackpressureAndFailures() {
    int serial[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    std::vector<unsigned char> expected(1024 * 1024);
    unsigned long i;
    for (i = 0; i < expected.size(); ++i) expected[i] = static_cast<unsigned char>(i * 37);
    assert(GatewayIntegrationTestPeer::queueSerialBytes(&gateway, expected) > 0);
    std::vector<unsigned char> received;
    unsigned int attempts;
    for (attempts = 0; attempts < 100 && received.size() < expected.size(); ++attempts) {
        std::vector<unsigned char> part = readAvailable(serial[1]);
        received.insert(received.end(), part.begin(), part.end());
        gateway.poll();
    }
    std::vector<unsigned char> tail = readAvailable(serial[1]);
    received.insert(received.end(), tail.begin(), tail.end());
    assert(received == expected);

    struct sigaction action;
    struct sigaction oldAction;
    action.sa_handler = noteSerialIoSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    assert(sigaction(SIGALRM, &action, &oldAction) == 0);
    int flags = fcntl(serial[0], F_GETFL);
    assert(flags >= 0 && fcntl(serial[0], F_SETFL, flags & ~O_NONBLOCK) == 0);
    serialIoSignalSeen = 0;
    alarm(1);
    gateway.poll();
    assert(serialIoSignalSeen && gateway.serialFd() == serial[0]);
    assert(fcntl(serial[0], F_SETFL, flags) == 0);
    close(serial[1]);
    gateway.poll();
    assert(gateway.serialFd() == -1);
    assert(gateway.logsJson().find("Serial read failed: end of file") != std::string::npos);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway interruptedWrite("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&interruptedWrite, serial[0]);
    unsigned char filler[4096];
    memset(filler, 0x3c, sizeof(filler));
    while (write(serial[0], filler, sizeof(filler)) > 0) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(GatewayIntegrationTestPeer::queueSerialBytes(&interruptedWrite,
        std::vector<unsigned char>(1, 0x55)) == 1);
    flags = fcntl(serial[0], F_GETFL);
    assert(flags >= 0 && fcntl(serial[0], F_SETFL, flags & ~O_NONBLOCK) == 0);
    serialIoSignalSeen = 0;
    alarm(1);
    GatewayIntegrationTestPeer::flushSerialOutput(&interruptedWrite);
    assert(serialIoSignalSeen && GatewayIntegrationTestPeer::pendingSerialBytes(interruptedWrite) == 1);
    assert(fcntl(serial[0], F_SETFL, flags) == 0);
    readAvailable(serial[1]);
    interruptedWrite.poll();
    std::vector<unsigned char> retried = readAvailable(serial[1]);
    assert(retried.size() == 1 && retried[0] == 0x55);
    interruptedWrite.stop();
    close(serial[1]);
    assert(sigaction(SIGALRM, &oldAction, 0) == 0);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway writeFailure("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&writeFailure, serial[0]);
    close(serial[1]);
    signal(SIGPIPE, SIG_IGN);
    GatewayIntegrationTestPeer::forceSerialWrite(&writeFailure);
    assert(writeFailure.serialFd() == -1);
    assert(writeFailure.logsJson().find("Serial write failed:") != std::string::npos);
}

void testAshAcknowledgementsRetriesAndDuplicates() {
    int serial[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    GatewayIntegrationTestPeer::queueEzspCommand(&gateway, 0x0005);
    gateway.poll();
    std::vector<unsigned char> originalWire = readAvailable(serial[1]);
    std::vector<unsigned char> original = decodeSingleFrame(originalWire);
    assert(original[0] == 0x00 && original.size() >= 4);
    sendAshControl(serial[1], 0xa0, std::vector<unsigned char>());
    gateway.poll();
    std::vector<unsigned char> nakRetry = decodeSingleFrame(readAvailable(serial[1]));
    assert(nakRetry[0] == 0x08 && std::vector<unsigned char>(nakRetry.begin() + 1, nakRetry.end()) ==
           std::vector<unsigned char>(original.begin() + 1, original.end()));
    sendAshControl(serial[1], 0x81, std::vector<unsigned char>());
    gateway.poll();

    GatewayIntegrationTestPeer::setAshSendSequence(&gateway, 7);
    GatewayIntegrationTestPeer::queueEzspCommand(&gateway, 0x0005);
    GatewayIntegrationTestPeer::queueEzspCommand(&gateway, 0x0005);
    gateway.poll();
    std::vector<unsigned char> sequenceSeven = decodeSingleFrame(readAvailable(serial[1]));
    assert(sequenceSeven[0] == 0x70);
    sendAshControl(serial[1], 0x80, std::vector<unsigned char>());
    gateway.poll();
    std::vector<unsigned char> wrappedSequence = decodeSingleFrame(readAvailable(serial[1]));
    assert(wrappedSequence[0] == 0x00);
    GatewayIntegrationTestPeer::expireAshAck(&gateway);
    gateway.poll();
    assert(decodeSingleFrame(readAvailable(serial[1]))[0] == 0x08);
    GatewayIntegrationTestPeer::expireAshAck(&gateway); gateway.poll(); readAvailable(serial[1]);
    GatewayIntegrationTestPeer::expireAshAck(&gateway); gateway.poll(); readAvailable(serial[1]);
    GatewayIntegrationTestPeer::expireAshAck(&gateway); gateway.poll();
    std::vector<unsigned char> resetWire = readAvailable(serial[1]);
    assert(resetWire.size() >= 2 && resetWire[0] == 0x1a && resetWire[1] == 0xc0);
    assert(gateway.logsJson().find("ASH acknowledgement failed repeatedly") != std::string::npos);

    std::vector<unsigned char> incomingEzsp;
    incomingEzsp.push_back(0); incomingEzsp.push_back(0x80); incomingEzsp.push_back(0xfe);
    sendAshControl(serial[1], 0x00, obfuscateEzsp(incomingEzsp));
    gateway.poll(); readAvailable(serial[1]);
    std::string afterFirst = gateway.logsJson();
    sendAshControl(serial[1], 0x08, obfuscateEzsp(incomingEzsp));
    gateway.poll();
    std::vector<unsigned char> duplicateAck = decodeSingleFrame(readAvailable(serial[1]));
    assert(duplicateAck[0] == 0x81);
    std::string afterDuplicate = gateway.logsJson();
    assert(afterDuplicate.find("ASH duplicate suppressed") != std::string::npos);
    size_t firstApply = afterFirst.find("EZSP <- v4 0xfe");
    assert(firstApply != std::string::npos && afterDuplicate.find("EZSP <- v4 0xfe", firstApply + 1) == std::string::npos);
    gateway.stop(); close(serial[1]);
}

void testSceneCommandErrors() {
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    assert(GatewayIntegrationTestPeer::sceneCommandErrors(&gateway));
}

void testSceneConfigurationProvisioning() {
    char name[] = "/tmp/microhad-zigbee-scenes-integration-XXXXXX";
    char* temporary = mkdtemp(name);
    assert(temporary != 0);
    std::string directory = temporary;
    Gateway gateway("/dev/null", 115200, directory, "", "", "");
    assert(GatewayIntegrationTestPeer::provisionSceneConfiguration(&gateway, directory));
    unlink((directory + "/scenes.conf").c_str());
    rmdir(directory.c_str());
}

void testSecurityDiagnosticsRedactAndBoundFallback() {
    char name[] = "/tmp/microhad-zigbee-diagnostics-XXXXXX";
    char* temporary = mkdtemp(name);
    int serial[2];
    std::string error;
    assert(temporary != 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
    const std::string directory = temporary;
    Gateway gateway("/dev/null", 115200, directory, "", "", "");
    GatewayIntegrationTestPeer::attachSerial(&gateway, serial[0]);
    assert(gateway.start(&error));
    GatewayIntegrationTestPeer::sendSensitiveSecurityState(&gateway);
    std::string logs = gateway.logsJson();
    assert(logs.find("SERIAL TX redacted sensitive frame bytes=") != std::string::npos);
    assert(logs.find("5a5a5a5a5a5a5a5a") == std::string::npos &&
           logs.find("a5a5a5a5a5a5a5a5") == std::string::npos);
    assert(!readAvailable(serial[1]).empty());
    GatewayIntegrationTestPeer::logBoundedUnknownFrame(&gateway);
    logs = gateway.logsJson();
    assert(logs.find("command=0xfe") != std::string::npos && logs.find("payload=112afea5") != std::string::npos);
    assert(logs.size() < 900);
    gateway.stop(); close(serial[1]);
    rmdir(directory.c_str());
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

void testSerialStartupAndIdempotentStop() {
    char name[] = "/tmp/microhad-zigbee-serial-lifecycle-XXXXXX";
    char* temporary = mkdtemp(name);
    assert(temporary != 0);
    const std::string directory = temporary;
    {
        Gateway missing(directory + "/missing-serial", 115200, directory, "", "", "");
        std::string error;
        assert(!missing.start(&error) && error.find("cannot open Zigbee serial device") != std::string::npos);
        assert(missing.serialFd() == -1);
        missing.stop(); missing.stop();
    }
    {
        Gateway invalidBaud("/dev/null", 9600, directory, "", "", "");
        std::string error;
        assert(!invalidBaud.start(&error) && error.find("only the built-in NCP baud rate 115200 is supported") != std::string::npos);
        assert(invalidBaud.serialFd() == -1);
        invalidBaud.stop(); invalidBaud.stop();
    }
    {
        std::ofstream denied((directory + "/permission-denied-serial").c_str());
        assert(denied); denied << "not a serial device"; denied.close();
        assert(chmod((directory + "/permission-denied-serial").c_str(), 0000) == 0);
        Gateway permissionDenied(directory + "/permission-denied-serial", 115200, directory, "", "", "");
        std::string error;
        assert(!permissionDenied.start(&error) && error.find("cannot open Zigbee serial device") != std::string::npos);
        assert(permissionDenied.serialFd() == -1);
        permissionDenied.stop();
        assert(chmod((directory + "/permission-denied-serial").c_str(), 0600) == 0);
        unlink((directory + "/permission-denied-serial").c_str());
    }
    assert(mkdir((directory + "/serial-directory").c_str(), 0700) == 0);
    {
        Gateway invalidPath(directory + "/serial-directory", 115200, directory, "", "", "");
        std::string error;
        assert(!invalidPath.start(&error) && error.find("cannot open Zigbee serial device") != std::string::npos);
        assert(invalidPath.serialFd() == -1);
        invalidPath.stop();
    }
    for (int i = 0; i < 2; ++i) {
        int serial[2];
        std::string error;
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, serial) == 0);
        Gateway started("/dev/null", 115200, directory, "", "", "");
        GatewayIntegrationTestPeer::attachSerial(&started, serial[0]);
        assert(started.start(&error) && started.serialFd() == serial[0]);
        started.stop();
        assert(started.serialFd() == -1);
        started.stop();
        assert(fcntl(serial[0], F_GETFD) == -1);
        close(serial[1]);
    }
    rmdir((directory + "/serial-directory").c_str());
    unlink((directory + "/devices.json").c_str());
    unlink((directory + "/scenes.conf").c_str());
    unlink((directory + "/network.json").c_str());
    rmdir(directory.c_str());
}

void testCliUsageFailures() {
    expectUsageFailure(false);
    expectUsageFailure(true);
}
}

void runGatewayIntegrationTests() {
    fprintf(stderr, "integration: gateway reset and EZSP version over pipe\n");
    testResetAndVersionOverPipe();
    fprintf(stderr, "integration: gateway reset timeout retry over pipe\n");
    testResetTimeoutRetriesOverPipe();
    fprintf(stderr, "integration: EZSP version negotiation failure modes\n");
    testEzspVersionFailureModes();
    fprintf(stderr, "integration: EZSP failed initialization status\n");
    testEzspFailedInitializationStatus();
    fprintf(stderr, "integration: fresh network formation and persistence\n");
    testFreshNetworkFormationOverSerial();
    fprintf(stderr, "integration: network creation rejection, RNG, EZSP failure, and timeout\n");
    testNetworkCreationFailuresAndTimeout();
    fprintf(stderr, "integration: secure join preparation policy sequence\n");
    testSecureJoinPreparationSuccess();
    fprintf(stderr, "integration: secure join policy failures, timeouts, and retry\n");
    testSecureJoinPreparationFailuresAndRetries();
    fprintf(stderr, "integration: join extension, expiry, renewal, and close failure\n");
    testJoinWindowExtensionExpiryRenewalAndCloseFailure();
    fprintf(stderr, "integration: IAS enrollment and zone ID exhaustion\n");
    testIasEnrollmentAndZoneIdExhaustion();
    fprintf(stderr, "integration: device announce, rejoin, and node reuse\n");
    testDeviceAnnounceRejoinAndNodeReuse();
    fprintf(stderr, "integration: complete sensor interview sequence\n");
    testCompleteSensorInterview();
    fprintf(stderr, "integration: interview response matching and timeout\n");
    testInterviewResponseMatchingAndTimeout();
    fprintf(stderr, "integration: coordinator initialization, IEEE lookup, and Tradfri setup\n");
    testCoordinatorInitializationSequence();
    fprintf(stderr, "integration: coordinator initialization unexpected reply and timeout\n");
    testCoordinatorInitializationUnexpectedReplyAndTimeout();
    fprintf(stderr, "integration: APS callback validation and dispatch\n");
    testApsCallbackValidationAndDispatch();
    fprintf(stderr, "integration: serial backpressure recovery and terminal failures\n");
    testSerialBackpressureAndFailures();
    fprintf(stderr, "integration: ASH acknowledgements, retries, duplicate suppression, and wrap\n");
    testAshAcknowledgementsRetriesAndDuplicates();
    fprintf(stderr, "integration: scene command errors and state preservation\n");
    testSceneCommandErrors();
    fprintf(stderr, "integration: scene configuration persistence, provisioning, and snapshot\n");
    testSceneConfigurationProvisioning();
    fprintf(stderr, "integration: secret redaction and bounded frame diagnostics\n");
    testSecurityDiagnosticsRedactAndBoundFallback();
    fprintf(stderr, "integration: gateway runtime directory boundaries\n");
    testRuntimeDirectoryBoundaries();
    fprintf(stderr, "integration: serial startup failures and idempotent stop\n");
    testSerialStartupAndIdempotentStop();
    fprintf(stderr, "integration: gateway CLI usage failures\n");
    testCliUsageFailures();
}
