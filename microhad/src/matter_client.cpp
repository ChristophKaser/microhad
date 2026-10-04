#include "matter_client.h"
#include "matter_protocol.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sstream>
#include <time.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
const unsigned int ioTimeoutSeconds = 1;

typedef std::map<std::string, std::string> Fields;
// Deliberately limited to the ASCII names/enums, booleans, small unsigned
// integers and one nested result object used by status/observation schema 1.
// Arbitrary strings, escapes, arrays or extra fields must never reach HTTP.
class StatusJson {
public:
    explicit StatusJson(const std::string& text) : text_(text), at_(0) {}
    bool parse(Fields* fields) { return object(fields, 0) && whitespace() == text_.size(); }
private:
    const std::string& text_;
    unsigned long at_;
    unsigned long whitespace() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\n' ||
               text_[at_] == '\r' || text_[at_] == '\t')) ++at_;
        return at_;
    }
    bool take(char c) {
        whitespace();
        if (at_ >= text_.size() || text_[at_] != c) return false;
        ++at_; return true;
    }
    bool string(std::string* value) {
        if (!take('"')) return false;
        value->clear();
        while (at_ < text_.size() && text_[at_] != '"') {
            unsigned char c = text_[at_++];
            if (c < 0x20 || c > 0x7e || c == '\\' || value->size() >= 64) return false;
            *value += static_cast<char>(c);
        }
        return take('"');
    }
    bool object(Fields* fields, unsigned int depth) {
        if (depth > 1 || !take('{')) return false;
        whitespace();
        if (at_ < text_.size() && text_[at_] == '}') { ++at_; return true; }
        while (fields->size() < 12) {
            std::string key, value;
            if (!string(&key) || fields->count(key) || !take(':')) return false;
            whitespace();
            if (at_ >= text_.size()) return false;
            if (text_[at_] == '{') {
                unsigned long start = at_;
                Fields nested;
                if (!object(&nested, depth + 1)) return false;
                value = text_.substr(start, at_ - start);
            } else if (text_[at_] == '"') {
                if (!string(&value)) return false;
                value = '"' + value + '"';
            } else {
                unsigned long start = at_;
                while (at_ < text_.size() && text_[at_] != ',' && text_[at_] != '}' &&
                       text_[at_] != ' ' && text_[at_] != '\t' && text_[at_] != '\r' && text_[at_] != '\n') ++at_;
                value = text_.substr(start, at_ - start);
                if (value != "true" && value != "false") {
                    if (value.empty() || value.size() > 3 || (value.size() > 1 && value[0] == '0')) return false;
                    for (unsigned long i = 0; i < value.size(); ++i)
                        if (value[i] < '0' || value[i] > '9') return false;
                }
            }
            (*fields)[key] = value;
            whitespace();
            if (at_ < text_.size() && text_[at_] == '}') { ++at_; return true; }
            if (!take(',')) return false;
        }
        return false;
    }
};

bool booleanField(const Fields& fields, const char* name) {
    Fields::const_iterator it = fields.find(name);
    return it != fields.end() && (it->second == "true" || it->second == "false");
}
bool member(const std::string& value, const char* const* values, unsigned int count) {
    for (unsigned int i = 0; i < count; ++i) if (value == values[i]) return true;
    return false;
}
bool snapshotValid(const Fields& fields, bool thread) {
    static const char* const common[] = {"enabled", "available", "state"};
    static const char* const matterOnly[] = {"controller_initialized", "fabric_ready"};
    static const char* const threadOnly[] = {"configured", "attached", "linux_ipv6_or_tun", "zigbee_channel", "dataset_channel"};
    static const char* const states[] = {"\"awaiting_coordinator\"", "\"detached\"", "\"attaching\"", "\"attached\"", "\"channel_mismatch\"", "\"error\""};
    if (!booleanField(fields, "enabled") || !booleanField(fields, "available") || !fields.count("state")) return false;
    if (thread) {
        if (!booleanField(fields, "configured") || !booleanField(fields, "attached") ||
            !fields.count("linux_ipv6_or_tun") || fields.find("linux_ipv6_or_tun")->second != "false" ||
            !member(fields.find("state")->second, states, 6)) return false;
    } else if (!booleanField(fields, "controller_initialized") || !booleanField(fields, "fabric_ready") ||
               (fields.find("state")->second != "\"running\"" && fields.find("state")->second != "\"awaiting_coordinator\"")) return false;
    for (Fields::const_iterator it = fields.begin(); it != fields.end(); ++it) {
        if (!member(it->first, common, 3) && !member(it->first, thread ? threadOnly : matterOnly, thread ? 5 : 2)) return false;
        if (it->first == "zigbee_channel" || it->first == "dataset_channel") {
            if (it->second.size() != 2 || it->second[0] < '1' || it->second[0] > '2' ||
                it->second[1] < '0' || it->second[1] > '9') return false;
            int channel = (it->second[0] - '0') * 10 + it->second[1] - '0';
            if (channel < 11 || channel > 26) return false;
        }
    }
    return true;
}
bool responseValid(unsigned char opcode, const std::string& text) {
    Fields fields;
    if (!StatusJson(text).parse(&fields)) return false;
    if (opcode == 1 || opcode == 2) return snapshotValid(fields, opcode == 2);
    if (opcode != 9 || fields.size() != 3 || fields["schema"] != "1" || !booleanField(fields, "ok")) return false;
    if (fields["ok"] == "true") {
        Fields snapshot;
        return fields.count("result") && StatusJson(fields["result"]).parse(&snapshot) && snapshotValid(snapshot, true);
    }
    static const char* const errors[] = {"\"malformed\"", "\"not_ready\"", "\"busy\"", "\"transport\"", "\"storage\""};
    return fields.count("error") && member(fields["error"], errors, 5);
}

int64_t nowMilliseconds() {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

bool waitFor(int fd, bool writing, int64_t deadline) {
    while (true) {
        int64_t now = nowMilliseconds();
        if (now < 0 || fd < 0 || fd >= FD_SETSIZE) return false;
        int64_t remaining = deadline - now;
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
        ssize_t written = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (written > 0) offset += static_cast<unsigned int>(written);
        else if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
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
        else if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        else return false;
    }
    return true;
}
}

MatterClient::MatterClient(const std::string& socketPath) : socketPath_(socketPath) {
}

bool MatterClient::status(bool thread, std::string* json, std::string* error) const {
    return call(thread ? MatterWire::ThreadStatus : MatterWire::MatterStatus, "", json, error);
}

bool MatterClient::observe(unsigned char channel, bool online, std::string* json, std::string* error) const {
    if ((online && (channel < 11 || channel > 26)) ||
        (!online && channel != 0 && (channel < 11 || channel > 26))) {
        json->clear(); *error = "Invalid coordinator observation"; return false;
    }
    std::ostringstream body;
    body << "{\"schema\":1,\"action\":\"observe\",\"zigbee_channel\":"
         << static_cast<unsigned int>(channel) << ",\"zigbee_online\":" << (online ? "true" : "false") << '}';
    return call(9, body.str(), json, error);
}

bool MatterClient::call(unsigned char opcode, const std::string& body, std::string* json, std::string* error) const {
    json->clear(); error->clear();
    const int64_t now = nowMilliseconds();
    if (now < 0) { *error = "Matter service clock unavailable"; return false; }
    const int64_t deadline = now + ioTimeoutSeconds * 1000;
    struct stat pathInfo;
    if (socketPath_.empty() || socketPath_.size() >= sizeof(((struct sockaddr_un*)0)->sun_path) ||
        lstat(socketPath_.c_str(), &pathInfo) != 0 || !S_ISSOCK(pathInfo.st_mode) ||
        pathInfo.st_uid != geteuid() || (pathInfo.st_mode & 0077) != 0) {
        *error = "Matter service socket unavailable";
        return false;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { *error = "Matter service unavailable"; return false; }
    if (fd >= FD_SETSIZE || fcntl(fd, F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        close(fd); *error = "Matter service unavailable"; return false;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socketPath_.c_str(), socketPath_.size() + 1);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        // Linux Unix sockets report EAGAIN when the listen backlog is full;
        // that is not an in-progress connection and must not be mistaken for one.
        if (errno != EINPROGRESS || !waitFor(fd, true, deadline)) {
            close(fd); *error = "Matter service unavailable"; return false;
        }
        int connectionError = 0;
        socklen_t length = sizeof(connectionError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &connectionError, &length) != 0 || connectionError != 0) {
            close(fd); *error = "Matter service unavailable"; return false;
        }
    }
    return exchange(fd, opcode, body, deadline, json, error);
}

bool MatterClient::requestStatus(int fd, bool thread, std::string* json, std::string* error) const {
    return exchange(fd, thread ? MatterWire::ThreadStatus : MatterWire::MatterStatus, "",
                    nowMilliseconds() + ioTimeoutSeconds * 1000, json, error);
}

bool MatterClient::exchange(int fd, unsigned char opcode, const std::string& body, int64_t deadline,
                            std::string* json, std::string* error) const {
    json->clear(); error->clear();
    MatterWire::Frame request;
    request.kind = MatterWire::Frame::Request;
    request.opcode = opcode;
    request.payload.assign(body.begin(), body.end());
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
    const std::string bodyJson(response.payload.begin(), response.payload.end());
    if (!responseValid(opcode, bodyJson)) { *error = "Matter service response invalid"; return false; }
    *json = bodyJson;
    return true;
}
