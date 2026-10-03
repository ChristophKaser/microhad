#include "matter_client.h"
#include "matter_protocol.h"

#include <cerrno>
#include <cstring>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
const unsigned int ioTimeoutSeconds = 1;

int64_t nowMilliseconds() {
    struct timeval now;
    gettimeofday(&now, 0);
    return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

bool waitFor(int fd, bool writing, int64_t deadline) {
    while (true) {
        int64_t remaining = deadline - nowMilliseconds();
        if (remaining <= 0) return false;
        struct timeval timeout;
        timeout.tv_sec = static_cast<long>(remaining / 1000);
        timeout.tv_usec = static_cast<long>((remaining % 1000) * 1000);
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(fd, &ready);
        int result = select(fd + 1, writing ? 0 : &ready, writing ? &ready : 0, 0, &timeout);
        if (result > 0) return true;
        if (result == 0) return false;
        if (errno != EINTR) return false;
    }
}

bool writeAll(int fd, const unsigned char* data, unsigned int size, int64_t deadline) {
    unsigned int offset = 0;
    while (offset < size) {
        if (!waitFor(fd, true, deadline)) return false;
        ssize_t written = write(fd, data + offset, size - offset);
        if (written > 0) offset += static_cast<unsigned int>(written);
        else if (written < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

bool readAll(int fd, unsigned char* data, unsigned int size, int64_t deadline) {
    unsigned int offset = 0;
    while (offset < size) {
        if (!waitFor(fd, false, deadline)) return false;
        ssize_t received = read(fd, data + offset, size - offset);
        if (received > 0) offset += static_cast<unsigned int>(received);
        else if (received < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}
}

MatterClient::MatterClient(const std::string& socketPath) : socketPath_(socketPath) {
}

bool MatterClient::status(bool thread, std::string* json, std::string* error) const {
    struct stat pathInfo;
    if (socketPath_.empty() || socketPath_.size() >= sizeof(((struct sockaddr_un*)0)->sun_path) ||
        lstat(socketPath_.c_str(), &pathInfo) != 0 || !S_ISSOCK(pathInfo.st_mode) ||
        pathInfo.st_uid != geteuid() || (pathInfo.st_mode & 0077) != 0) {
        *error = "Matter service socket unavailable";
        return false;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { *error = "Matter service unavailable"; return false; }
    struct timeval timeout;
    timeout.tv_sec = ioTimeoutSeconds;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socketPath_.c_str(), socketPath_.size() + 1);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd); *error = "Matter service unavailable"; return false;
    }
    return requestStatus(fd, thread, json, error);
}

bool MatterClient::requestStatus(int fd, bool thread, std::string* json, std::string* error) const {
    const int64_t deadline = nowMilliseconds() + ioTimeoutSeconds * 1000;
    MatterWire::Frame request;
    request.kind = MatterWire::Frame::Request;
    request.opcode = thread ? MatterWire::ThreadStatus : MatterWire::MatterStatus;
    request.id = 1;
    MatterWire::Bytes encoded;
    if (!MatterWire::encode(request, &encoded) || !writeAll(fd, &encoded[0], encoded.size(), deadline)) {
        close(fd); *error = "Matter service request failed"; return false;
    }
    unsigned char header[10];
    if (!readAll(fd, header, sizeof(header), deadline)) {
        close(fd); *error = "Matter service response timed out"; return false;
    }
    unsigned int payloadSize = static_cast<unsigned int>(header[0]) |
                               (static_cast<unsigned int>(header[1]) << 8);
    if (payloadSize > MatterWire::MaxPayload) {
        close(fd); *error = "Matter service response invalid"; return false;
    }
    MatterWire::Bytes responseBytes(header, header + sizeof(header));
    responseBytes.resize(sizeof(header) + payloadSize);
    if (payloadSize != 0 && !readAll(fd, &responseBytes[sizeof(header)], payloadSize, deadline)) {
        close(fd); *error = "Matter service response timed out"; return false;
    }
    close(fd);

    MatterWire::Decoder decoder;
    if (!decoder.feed(responseBytes)) { *error = "Matter service response invalid"; return false; }
    MatterWire::Frame response;
    if (!decoder.pop(&response) || response.kind != MatterWire::Frame::Response ||
        response.id != request.id || response.opcode != request.opcode || response.payload.empty()) {
        *error = "Matter service response invalid";
        return false;
    }
    json->assign(response.payload.begin(), response.payload.end());
    return true;
}
