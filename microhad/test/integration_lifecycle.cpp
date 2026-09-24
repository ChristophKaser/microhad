#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>

namespace {
int unusedPort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    assert(fd >= 0);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0);
    assert(getsockname(fd, reinterpret_cast<struct sockaddr*>(&address), &length) == 0);
    int port = ntohs(address.sin_port);
    close(fd);
    return port;
}

void writeConfig(const std::string& filename, const std::string& serialPath,
                 const std::string& runtimePath, int port) {
    int fd = open(filename.c_str(), O_WRONLY | O_TRUNC);
    char buffer[512];
    int length = snprintf(buffer, sizeof(buffer),
        "{\"serial_device\":\"%s\",\"runtime_dir\":\"%s\",\"http_port\":%d}",
        serialPath.c_str(), runtimePath.c_str(), port);
    assert(fd >= 0 && length > 0 && static_cast<size_t>(length) < sizeof(buffer));
    assert(write(fd, buffer, static_cast<size_t>(length)) == length);
    close(fd);
}

pid_t startGateway(const std::string& config) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl("build/host/microhad-zigbee-native", "microhad-zigbee-native", "--config", config.c_str(), static_cast<char*>(0));
        _exit(127);
    }
    return child;
}

void waitForExit(pid_t child) {
    int status = 0;
    unsigned int attempt;
    for (attempt = 0; attempt < 200; ++attempt) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            return;
        }
        assert(result == 0);
        usleep(10000);
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    assert(false && "gateway did not exit after termination signal");
}

bool listenerReady(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    if (fd < 0) return false;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(port));
    bool ready = connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0;
    close(fd);
    return ready;
}

void verifyDaemonSignals(int signalNumber, bool exerciseBrokenPipe) {
    char directoryName[] = "/tmp/microhad-zigbee-signals-XXXXXX";
    char* directory = mkdtemp(directoryName);
    char configName[256];
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    std::string serialPath;
    int port = unusedPort();
    pid_t child;
    unsigned int attempt;
    assert(directory != 0 && master >= 0);
    assert(grantpt(master) == 0 && unlockpt(master) == 0 && ptsname(master) != 0);
    serialPath = ptsname(master);
    snprintf(configName, sizeof(configName), "%s/config.json", directory);
    {
        int fd = open(configName, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        assert(fd >= 0);
        close(fd);
    }
    writeConfig(configName, serialPath, directory, port);
    child = startGateway(configName);
    bool ready = false;
    for (attempt = 0; attempt < 200 && !ready; ++attempt) {
        int status;
        assert(waitpid(child, &status, WNOHANG) == 0);
        ready = listenerReady(port);
        if (!ready) usleep(10000);
    }
    assert(ready);
    usleep(50000);
    if (exerciseBrokenPipe) {
        for (attempt = 0; attempt < 20; ++attempt) {
            int client = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in address;
            struct linger reset;
            const char request[] = "GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n";
            assert(client >= 0);
            reset.l_onoff = 1; reset.l_linger = 0;
            assert(setsockopt(client, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(static_cast<unsigned short>(port));
            assert(connect(client, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0);
            assert(write(client, request, sizeof(request) - 1) == static_cast<ssize_t>(sizeof(request) - 1));
            close(client);
            usleep(10000);
        }
        int status;
        assert(waitpid(child, &status, WNOHANG) == 0);
    }
    assert(kill(child, signalNumber) == 0);
    waitForExit(child);
    close(master);
    unlink(configName);
    rmdir(directory);
}
}

void runLifecycleIntegrationTests() {
    verifyDaemonSignals(SIGINT, false);
    verifyDaemonSignals(SIGTERM, true);
    fprintf(stderr, "integration: daemon SIGINT/SIGTERM shutdown and SIGPIPE-safe HTTP writes\n");
}
