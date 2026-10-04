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
// Bounded canonical ASCII strings, booleans, unsigned integers and one
// nested object; typed validators below further restrict every body.
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
            if (c < 0x20 || c > 0x7e || c == '\\' || value->size() >= 488) return false;
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
                    if (value.empty() || value.size() > 10 || (value.size() > 1 && value[0] == '0')) return false;
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
bool number(const std::string& value, uint32_t minimum, uint32_t maximum) {
    if (value.empty() || value.size() > 10 || (value.size() > 1 && value[0] == '0')) return false;
    uint64_t result = 0;
    for (unsigned long i = 0; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
        result = result * 10 + value[i] - '0';
    }
    return result >= minimum && result <= maximum;
}
bool hex(const std::string& value, unsigned int minimum, unsigned int maximum) {
    if (value.size() < minimum + 2 || value.size() > maximum + 2 ||
        value[0] != '"' || value[value.size()-1] != '"' || (value.size() % 2)) return false;
    for (unsigned long i = 1; i + 1 < value.size(); ++i)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
    return true;
}
bool errorValid(const std::string& value) {
    static const char* const errors[] = {"\"malformed\"", "\"not_ready\"", "\"busy\"", "\"stale_session\"",
        "\"stale_generation\"", "\"timeout\"", "\"cancelled\"", "\"transport\"", "\"storage\"",
        "\"capacity\"", "\"accessory\"", "\"unsupported\""};
    return member(value, errors, 12);
}
bool sessionValid(Fields fields) {
    static const char* const states[] = {"\"waiting_ble\"", "\"pase\"", "\"provisioning\"", "\"waiting_thread\"",
        "\"case\"", "\"completing\"", "\"complete\"", "\"cancelled\"", "\"failed\"", "\"timed_out\""};
    unsigned int count = 3;
    if (fields.count("error")) { ++count; if (!errorValid(fields["error"]) || fields["error"] == "\"unsupported\"") return false; }
    if (fields.count("node")) { ++count; if (!hex(fields["node"], 16, 16) || fields["node"] == "\"0000000000000000\"") return false; }
    return fields.size() == count && hex(fields["session"], 16, 16) &&
        fields["session"] != "\"0000000000000000\"" && member(fields["state"], states, 10) &&
        number(fields["deadline_remaining_seconds"], 0, 900);
}
bool responseValid(unsigned char opcode, const std::string& text) {
    Fields fields;
    if (!StatusJson(text).parse(&fields)) return false;
    if (opcode == 1 || opcode == 2) return snapshotValid(fields, opcode == 2);
    if (opcode < 3 || opcode > 9 || fields.size() != 3 || fields["schema"] != "1" || !booleanField(fields, "ok")) return false;
    if (fields["ok"] == "false") return fields.count("error") && errorValid(fields["error"]);
    Fields result;
    if (!fields.count("result") || !StatusJson(fields["result"]).parse(&result)) return false;
    if (opcode == 9) return snapshotValid(result, true);
    if (opcode <= 5) return sessionValid(result);
    const char* name = opcode == 6 ? "generation" : opcode == 7 ? "sequence" : "write";
    return result.size() == 1 && number(result[name], 1, 0xffffffffU);
}
bool requestValid(unsigned char opcode, const std::string& text) {
    Fields fields;
    if (text.size() > MatterWire::MaxPayload || !StatusJson(text).parse(&fields) || fields["schema"] != "1") return false;
    if (opcode == 3) {
        static const char* const excluded[] = {"11111111", "22222222", "33333333", "44444444", "55555555",
            "66666666", "77777777", "88888888", "12345678", "87654321"};
        return fields.size() == 4 && number(fields["discriminator"], 0, 4095) &&
            number(fields["pin"], 1, 99999998) && !member(fields["pin"], excluded, 10) &&
            number(fields["timeout_seconds"], 30, 900);
    }
    if (opcode < 4 || opcode > 8 || !hex(fields["session"], 16, 16) ||
        fields["session"] == "\"0000000000000000\"") return false;
    if (opcode == 4) return fields.size() == (fields.count("subscribe") ? 3U : 2U) &&
        (!fields.count("subscribe") || booleanField(fields, "subscribe"));
    if (opcode == 5) return fields.size() == 2;
    if (opcode == 6) return fields.size() == 4 && hex(fields["address"], 12, 12) && number(fields["mtu"], 23, 517);
    if (!number(fields["generation"], 1, 0xffffffffU)) return false;
    if (opcode == 7) return fields.size() == 5 && number(fields["sequence"], 1, 0xffffffffU) && hex(fields["data"], 2, 488);
    return fields.size() == 5 && number(fields["write"], 1, 0xffffffffU) && booleanField(fields, "success");
}

bool eventValid(const std::string& text) {
    Fields fields, data;
    if (!StatusJson(text).parse(&fields) || fields.size() != 3 || fields["schema"] != "1" ||
        !fields.count("data") || !StatusJson(fields["data"]).parse(&data)) return false;
    if (fields["event"] == "\"thread\"") return snapshotValid(data, true);
    if (fields["event"] == "\"commissioning\"") return sessionValid(data);
    return fields["event"] == "\"ble_write\"" && data.size() == 4 && hex(data["session"], 16, 16) &&
        data["session"] != "\"0000000000000000\"" && number(data["generation"], 1, 0xffffffffU) &&
        number(data["write"], 1, 0xffffffffU) && hex(data["data"], 2, 488);
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
int connectPrivate(const std::string& socketPath, int64_t deadline, std::string* error) {
    struct stat pathInfo;
    if (socketPath.empty() || socketPath[0] != '/' || socketPath.size() >= sizeof(((struct sockaddr_un*)0)->sun_path) ||
        lstat(socketPath.c_str(), &pathInfo) != 0 || !S_ISSOCK(pathInfo.st_mode) ||
        pathInfo.st_uid != geteuid() || (pathInfo.st_mode & 0077) != 0) {
        *error = "Matter service socket unavailable";
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { *error = "Matter service unavailable"; return -1; }
    if (fd >= FD_SETSIZE || fcntl(fd, F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        close(fd); *error = "Matter service unavailable"; return -1;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        // Linux Unix sockets report EAGAIN when the listen backlog is full;
        // that is not an in-progress connection and must not be mistaken for one.
        if (errno != EINPROGRESS || !waitFor(fd, true, deadline)) {
            close(fd); *error = "Matter service unavailable"; return -1;
        }
        int connectionError = 0;
        socklen_t length = sizeof(connectionError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &connectionError, &length) != 0 || connectionError != 0) {
            close(fd); *error = "Matter service unavailable"; return -1;
        }
    }
    return fd;
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
    int fd = connectPrivate(socketPath_, deadline, error);
    if (fd < 0) return false;
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

MatterChannel::MatterChannel(const std::string& path) : socketPath_(path), fd_(-1), nextId_(1) {}
MatterChannel::~MatterChannel() { disconnect(); }
void MatterChannel::disconnect() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    decoder_ = MatterWire::Decoder();
    events_.clear();
}
bool MatterChannel::receive(int64_t deadline, bool wait) {
    if (fd_ < 0 || (wait && !waitFor(fd_, false, deadline))) return false;
    unsigned char buffer[MatterWire::MaxChunk];
    ssize_t count;
    do { count = recv(fd_, buffer, sizeof(buffer), MSG_DONTWAIT); } while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return !wait;
    return count > 0 && decoder_.feed(MatterWire::Bytes(buffer, buffer + count));
}
bool MatterChannel::event(const MatterWire::Frame& frame) {
    const std::string text(frame.payload.begin(), frame.payload.end());
    if (frame.kind != MatterWire::Frame::Event || frame.opcode != 12 || frame.id != 0 ||
        events_.size() >= 8 || !eventValid(text)) return false;
    events_.push_back(text);
    return true;
}
bool MatterChannel::popEvent(std::string* json) {
    json->clear();
    if (events_.empty()) return false;
    *json = events_.front(); events_.pop_front(); return true;
}
bool MatterChannel::poll(std::string* error) {
    error->clear();
    if (!receive(0, false)) { disconnect(); *error = "Matter service unavailable"; return false; }
    MatterWire::Frame frame;
    while (decoder_.pop(&frame)) {
        if (!event(frame)) { disconnect(); *error = "Matter service event invalid"; return false; }
    }
    return true;
}
bool MatterChannel::call(unsigned char opcode, const std::string& body, std::string* json, std::string* error) {
    json->clear(); error->clear();
    int64_t now = nowMilliseconds();
    if (now < 0 || nextId_ == 0 || opcode < 1 || opcode > 9 || body.size() > MatterWire::MaxPayload) {
        *error = "Matter service request invalid"; return false;
    }
    const int64_t deadline = now + 1000;
    if (fd_ < 0) fd_ = connectPrivate(socketPath_, deadline, error);
    if (fd_ < 0) return false;
    MatterWire::Frame request;
    request.kind = MatterWire::Frame::Request; request.opcode = opcode; request.id = nextId_++;
    request.payload.assign(body.begin(), body.end());
    MatterWire::Bytes bytes;
    bool valid = MatterWire::encode(request, &bytes) && writeAll(fd_, &bytes[0], bytes.size(), deadline);
    while (valid) {
        MatterWire::Frame frame;
        while (decoder_.pop(&frame)) {
            if (frame.kind == MatterWire::Frame::Event) { if (!event(frame)) { valid = false; break; } }
            else {
                const std::string text(frame.payload.begin(), frame.payload.end());
                if (frame.kind != MatterWire::Frame::Response || frame.id != request.id ||
                    frame.opcode != opcode || !responseValid(opcode, text)) { valid = false; break; }
                *json = text; return true;
            }
        }
        if (valid) valid = receive(deadline, true);
    }
    disconnect(); *error = "Matter service exchange failed"; return false;
}

bool MatterChannel::validRequest(unsigned char opcode, const std::string& body) {
    return requestValid(opcode, body);
}
