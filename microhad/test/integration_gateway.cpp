#include "ash.h"
#include "gateway.h"
#include "gateway_integration_peer.h"

#include <cassert>
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
