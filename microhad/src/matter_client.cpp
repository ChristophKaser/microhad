#include "matter_client.h"
#include "matter_json.h"
#include "matter_typed.h"
#include "bounded_json.h"
#include "json_util.h"
#include "thermostat.h"
#include "matter_protocol.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
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
#include <syslog.h>
#include <unistd.h>

namespace {
const unsigned int ioTimeoutSeconds = 1;

typedef std::map<std::string, std::string> Fields;
// Bounded JSON syntax; operation-specific validators restrict every field.
// Arrays are bounded and used only by typed device snapshot replies.
typedef BoundedJson StatusJson;

bool booleanField(const Fields& fields, const char* name) {
    Fields::const_iterator it = fields.find(name);
    return it != fields.end() && (it->second == "true" || it->second == "false");
}
bool member(const std::string& value, const char* const* values, unsigned int count) {
    for (unsigned int i = 0; i < count; ++i) if (value == values[i]) return true;
    return false;
}
bool hex(const std::string& value, unsigned int minimum, unsigned int maximum);

bool number(const std::string& value, uint32_t minimum, uint32_t maximum) {
    if (value.empty() || value.size() > 10 || (value.size() > 1 && value[0] == '0')) return false;
    uint64_t result = 0;
    for (unsigned long i = 0; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
        result = result * 10 + value[i] - '0';
    }
    return result >= minimum && result <= maximum;
}
bool signedNumber(const std::string& value, int32_t minimum, int32_t maximum) {
    if (value.empty() || value.size() > 11) return false;
    unsigned long start = value[0] == '-' ? 1 : 0;
    if (start == value.size() || (value.size() - start > 1 && value[start] == '0')) return false;
    uint32_t result = 0;
    for (unsigned long i = start; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
        result = result * 10 + value[i] - '0';
        if (result > 50400U) return false;
    }
    const int32_t signedResult = start ? -static_cast<int32_t>(result) : static_cast<int32_t>(result);
    return signedResult >= minimum && signedResult <= maximum;
}



bool hex(const std::string& value, unsigned int minimum, unsigned int maximum) {
    if (value.size() < minimum + 2 || value.size() > maximum + 2 ||
        value[0] != '"' || value[value.size()-1] != '"' || (value.size() % 2)) return false;
    for (unsigned long i = 1; i + 1 < value.size(); ++i)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
    return true;
}







bool requestValid(unsigned char opcode, const std::string& text) {
    Fields fields;
    if (text.size() > MatterWire::MaxPayload || !StatusJson(text).parse(&fields) || fields["schema"] != "1") return false;
    if (opcode == 10) {
        if (fields["action"] == "\"subscriptions\"") return fields.size() == 5 &&
            hex(fields["node"], 16, 16) && fields["node"] > "\"0000000000000001\"" &&
            booleanField(fields, "restore") && number(fields["cursor"], 0, 384) &&
            (fields["restore"] == "false" || fields["cursor"] == "0");
        if (fields["action"] == "\"delete\"") return fields.size() == 4 && hex(fields["node"], 16, 16) &&
            fields["node"] > "\"0000000000000001\"" && booleanField(fields, "local");
        if(fields["action"]=="\"read\"" && fields["cluster"]=="513") return fields.size()==6 && hex(fields["node"],16,16) && fields["node"]>"\"0000000000000001\"" &&
            number(fields["endpoint"],0,65534) && number(fields["attribute"],0,65542) &&
            (thermostatWireValue(static_cast<unsigned int>(strtoul(fields["attribute"].c_str(),0,10)),"thermostat","null") || fields["attribute"]=="65529" || fields["attribute"]=="65531" || number(fields["attribute"],65536,65542));
        if (fields["action"] == "\"read\"") return fields.size() == 6 &&
            hex(fields["node"], 16, 16) && fields["node"] > "\"0000000000000001\"" &&
            number(fields["endpoint"], 0, 65534) &&
            (fields["cluster"] == "6" || fields["cluster"] == "8" || fields["cluster"] == "69" || fields["cluster"] == "91" ||
             fields["cluster"] == "1026" || fields["cluster"] == "1029" ||
             fields["cluster"] == "1037" || fields["cluster"] == "1066") && fields["attribute"] == "0";
    if (fields["action"] == "\"bind\"") return fields.size() == 5 &&
            hex(fields["node"], 16, 16) && fields["node"] > "\"0000000000000001\"" &&
            number(fields["endpoint"], 0, 65534) && fields["targets"].size() >= 3 &&
            fields["targets"][0] == '[' && fields["targets"][fields["targets"].size() - 1] == ']';
        if (fields["action"] == "\"job\"") return fields.size() == 3 &&
            hex(fields["job"], 16, 16) && fields["job"] != "\"0000000000000000\"";
        return false;
    }
    if (opcode == 11) {
        if(fields["command"]=="\"thermostat\"") {
            std::string field,value;
            return fields.size()==6 && hex(fields["node"],16,16) && fields["node"]>"\"0000000000000001\"" && number(fields["endpoint"],0,65534) &&
                jsonStringField(text,"field",&field) && jsonStringField(text,"value",&value) && value.size()<=1536 &&
                (thermostatAttribute(field)!=0 || (field.size()==10 && field.compare(0,9,"schedule_")==0 && field[9]>='0' && field[9]<='6'));
        }
        static const char* const commands[] = {"\"on\"", "\"off\"", "\"toggle\"", "\"sync_time\"", "\"identify\""};
        if (!hex(fields["node"], 16, 16) || fields["node"] <= "\"0000000000000001\"" ||
            !number(fields["endpoint"], 0, 65534)) return false;
        if (fields["command"] == "\"sync_time\"") return fields.size() == 6 && signedNumber(fields["offset_seconds"], -50400, 50400) && fields["time_zone"].size() > 2;
        if (member(fields["command"], commands, 5)) return fields.size() == 4;
        return fields["command"] == "\"move_to_level\"" && fields.size() == 6 &&
            number(fields["level"], 0, 254) && number(fields["transition_deciseconds"], 0, 65535);
    }
    if (opcode == 9) {
        if (fields["action"] == "\"create\"" || fields["action"] == "\"recreate\"") return fields.size() == 2;
        if (fields["action"] == "\"retry\"") return fields.size() == 2;
        return fields["action"] == "\"import\"" && fields.size() == 3 && hex(fields["dataset"], 2, 508);
    }
    if (opcode == 3) {
        static const char* const excluded[] = {"11111111", "22222222", "33333333", "44444444", "55555555",
            "66666666", "77777777", "88888888", "12345678", "87654321"};
        return fields.size() == 6 && number(fields["discriminator"], 0, 4095) &&
            number(fields["pin"], 1, 99999998) && !member(fields["pin"], excluded, 10) &&
            number(fields["timeout_seconds"], 30, 900) &&
            fields["country_code"].size() == 4 && fields["country_code"][0] == '"' &&
            fields["country_code"][3] == '"' && fields["country_code"][1] >= 'A' &&
            fields["country_code"][1] <= 'Z' && fields["country_code"][2] >= 'A' &&
            fields["country_code"][2] <= 'Z' &&
            (fields["regulatory_location"] == "\"indoor\"" || fields["regulatory_location"] == "\"outdoor\"");
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
        ssize_t written = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (written > 0) offset += static_cast<unsigned int>(written);
        else if (written < 0 && errno == EINTR) continue;
        else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!waitFor(fd, true, deadline)) return false;
        }
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

bool MatterChannel::commissioningStartRequest(const std::string& input, const std::string& countryCode,
                                               const std::string& location, std::string* body) {
    Fields fields;
    if (!body || countryCode.size() != 2 || countryCode[0] < 'A' || countryCode[0] > 'Z' ||
        countryCode[1] < 'A' || countryCode[1] > 'Z' ||
        (location != "indoor" && location != "outdoor") ||
        !StatusJson(input).parse(&fields) || fields.size() != 4 || fields["schema"] != "1" ||
        !number(fields["discriminator"], 0, 4095) || !number(fields["pin"], 1, 99999998) ||
        !number(fields["timeout_seconds"], 30, 900)) return false;
    std::ostringstream prepared;
    prepared << "{\"schema\":1,\"discriminator\":" << fields["discriminator"]
             << ",\"pin\":" << fields["pin"] << ",\"timeout_seconds\":" << fields["timeout_seconds"]
             << ",\"country_code\":\"" << countryCode << "\",\"regulatory_location\":\""
             << location << "\"}";
    *body = prepared.str();
    return validRequest(3, *body);
}

MatterClient::MatterClient(const std::string& socketPath) : socketPath_(socketPath) {
}

bool MatterClient::status(bool thread, std::string* json, std::string* error) const {
    MatterProto::MessageBody body;if(thread)body.thread_status_request.present=true;else body.matter_status_request.present=true;
    return call(thread?MatterWire::ThreadStatus:MatterWire::MatterStatus,body,json,error);
}

bool MatterClient::observe(unsigned char channel,bool online,std::string* json,std::string* error) const {
    json->clear();error->clear();
    if((online&&(channel<11||channel>26))||(!online&&channel!=0&&(channel<11||channel>26))){*error="Invalid coordinator observation";return false;}
    const int64_t deadline=nowMilliseconds()+ioTimeoutSeconds*1000;
    const int fd=connectPrivate(socketPath_,deadline,error);if(fd<0)return false;
    MatterProto::ThreadControlRequest observation;observation.schema=1;observation.action=std::string("observe");observation.zigbee_channel=channel;observation.zigbee_online=online;
    MatterProto::MessageBody body;body.thread_control_request=observation;
    return exchangeTyped(fd,9,body,deadline,json,error);
}

bool MatterClient::call(unsigned char opcode,const MatterProto::MessageBody& body,std::string* json,std::string* error) const {
    json->clear(); error->clear();
    const int64_t now = nowMilliseconds();
    if (now < 0) { *error = "Matter service clock unavailable"; return false; }
    const int64_t deadline = now + ioTimeoutSeconds * 1000;
    int fd = connectPrivate(socketPath_, deadline, error);
    if (fd < 0) return false;
    return exchangeTyped(fd,opcode,body,deadline,json,error);
}

bool MatterClient::requestStatus(int fd,bool thread,std::string* json,std::string* error) const {
    MatterProto::MessageBody body;if(thread)body.thread_status_request.present=true;else body.matter_status_request.present=true;
    return exchangeTyped(fd,thread?MatterWire::ThreadStatus:MatterWire::MatterStatus,body,nowMilliseconds()+ioTimeoutSeconds*1000,json,error);
}
#ifdef MICROHAD_TESTING
bool MatterClient::exchange(int fd, unsigned char opcode, const std::string& body, int64_t deadline,
                            std::string* json, std::string* error) const {
    MatterProto::MessageBody request;
    if(!MatterWire::parseJson(0,opcode,body,&request)){close(fd);*error="Matter service request invalid";return false;}
    return exchangeTyped(fd,opcode,request,deadline,json,error);
}
#endif
bool MatterClient::exchangeTyped(int fd,unsigned char opcode,const MatterProto::MessageBody& body,int64_t deadline,std::string* json,std::string* error) const {
    json->clear(); error->clear();
    MatterWire::Frame request;
    request.kind = MatterWire::Frame::Request;
    request.opcode = opcode;
    request.payload=body;
    request.id = 1;
    MatterWire::Bytes encoded;
    if (!MatterWire::encode(request, &encoded) || !writeAll(fd, &encoded[0], encoded.size(), deadline)) {
        close(fd); *error = "Matter service request failed"; return false;
    }
    unsigned char header[2];
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
        response.id != request.id || response.opcode != request.opcode) {
        *error = "Matter service response invalid";
        return false;
    }
    if(!MatterWire::validResponse(opcode,response.payload)){*error="Matter service response invalid";return false;}
    *json=MatterWire::jsonBody(1,opcode,response.payload);
    return true;
}

MatterChannel::MatterChannel(const std::string& path) : socketPath_(path), fd_(-1), nextId_(1),
    eventHandler_(0), eventHandlerOpaque_(0) {}
MatterChannel::~MatterChannel() { disconnect(); }
void MatterChannel::setEventHandler(EventHandler handler, void* opaque) {
    eventHandler_ = handler; eventHandlerOpaque_ = opaque;
}
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
    if (count <= 0) {
        syslog(LOG_WARNING, "Matter IPC receive failed: reason=%s errno=%d", count == 0 ? "peer_closed" : "read_error", count < 0 ? errno : 0);
        return false;
    }
    if (!decoder_.feed(MatterWire::Bytes(buffer, buffer + count))) {
        syslog(LOG_WARNING, "Matter IPC receive failed: reason=malformed_frame");
        return false;
    }
    return true;
}
bool MatterChannel::event(const MatterWire::Frame& frame) {
    if (frame.kind != MatterWire::Frame::Event || frame.opcode != 12 || frame.id != 0 ||
        events_.size() >= 32 || !MatterWire::validEvent(frame.payload)) {
        syslog(LOG_WARNING, "Matter IPC event rejected: reason=%s opcode=%u queued=%lu",
            events_.size() >= 32 ? "queue_full" : "invalid_event", static_cast<unsigned int>(frame.opcode),
            static_cast<unsigned long>(events_.size()));
        return false;
    }
    events_.push_back(frame.payload);
    return eventHandler_ == 0 || eventHandler_(eventHandlerOpaque_);
}
#ifdef MICROHAD_TESTING
bool MatterChannel::popEvent(std::string* json) {
    json->clear();
    if (events_.empty()) return false;
    *json=MatterWire::jsonBody(2,12,events_.front());events_.pop_front();return true;
}
#endif
bool MatterChannel::poll(std::string* error) {
    error->clear();
    if (!receive(0, false)) { disconnect(); *error = "Matter service unavailable"; return false; }
    MatterWire::Frame frame;
    while (decoder_.pop(&frame)) {
        if (!event(frame)) { disconnect(); *error = "Matter service event invalid"; return false; }
    }
    return true;
}
#ifdef MICROHAD_TESTING
bool MatterChannel::call(unsigned char opcode,const std::string& input,std::string* json,std::string* error){
    Body body,response;json->clear();
    if(!MatterWire::parseJson(0,opcode,input,&body)){*error="Matter service request invalid";return false;}
    if(!call(opcode,body,&response,error))return false;
    *json=MatterWire::jsonBody(1,opcode,response);return true;
}
#endif
bool MatterChannel::popEvent(Body* body){if(events_.empty())return false;*body=events_.front();events_.pop_front();return true;}
bool MatterChannel::call(unsigned char opcode,const Body& body,Body* response,std::string* error) {
    *response=Body();error->clear();
    int64_t now = nowMilliseconds();
    if (now < 0 || nextId_ == 0 || opcode < 1 || opcode > 11) {
        *error = "Matter service request invalid"; return false;
    }
    // CASE cryptography can occupy the single-threaded MIPS service for more
    // than a second. Closing this owned IPC channel cancels commissioning.
    const int64_t deadline = now + 10000;
    if (fd_ < 0) fd_ = connectPrivate(socketPath_, deadline, error);
    if (fd_ < 0) return false;
    MatterWire::Frame request;
    request.kind = MatterWire::Frame::Request; request.opcode = opcode; request.id = nextId_++;
    request.payload=body;
    MatterWire::Bytes bytes;
    bool valid = MatterWire::encode(request, &bytes) && writeAll(fd_, &bytes[0], bytes.size(), deadline);
    const char* failure = "write_failed";
    while (valid) {
        MatterWire::Frame frame;
        while (decoder_.pop(&frame)) {
            if (frame.kind == MatterWire::Frame::Event) { if (!event(frame)) { failure = "invalid_event"; valid = false; break; } }
            else {
                if (frame.kind != MatterWire::Frame::Response || frame.id != request.id ||
                    frame.opcode != opcode || !MatterWire::validResponse(opcode,frame.payload)) { failure = "invalid_response"; valid = false; break; }
                *response=frame.payload;return true;
            }
        }
        if (valid) { failure = "receive_failed"; valid = receive(deadline, true); }
    }
    const int64_t elapsed = nowMilliseconds() - now;
    syslog(LOG_WARNING, "Matter IPC disconnect: opcode=%u reason=%s elapsed_ms=%ld queued=%lu",
        static_cast<unsigned int>(opcode), elapsed >= 10000 ? "timeout" : failure,
        static_cast<long>(elapsed), static_cast<unsigned long>(events_.size()));
    disconnect(); *error = "Matter service exchange failed"; return false;
}

bool MatterChannel::validRequest(unsigned char opcode, const std::string& body) {
    return requestValid(opcode, body);
}
#ifdef MICROHAD_TESTING
bool MatterChannel::acceptedOperationJob(unsigned char opcode,const std::string& input,std::string* id){Body b;uint64_t raw=0;const bool ok=MatterWire::parseJson(1,opcode,input,&b)&&acceptedOperationJob(opcode,b,&raw);*id=ok?MatterWire::hexId(raw):"";return ok;}
#endif

#ifdef MICROHAD_TESTING
bool MatterChannel::operationStatus(const std::string& input,const std::string& id,bool* terminal,bool* failed,std::string* error){Body b;return MatterWire::parseJson(1,10,input,&b)&&operationStatus(b,strtoull(id.c_str(),0,16),terminal,failed,error);}
#endif

#ifdef MICROHAD_TESTING
std::string MatterChannel::operationPollRequest(const std::string& job) {
    if (!hex('"' + job + '"', 16, 16) || job == "0000000000000000") return std::string();
    return "{\"schema\":1,\"action\":\"job\",\"job\":\"" + job + "\"}";
}
#endif

bool MatterChannel::threadMutation(const std::string& action, const std::string& input, std::string* body) {
    body->clear();
    const bool importing = action == "import";
    const bool retrying = action == "retry";
    const bool recreating = action == "recreate";
    if (action != "create" && !importing && !retrying && !recreating) return false;
    Fields fields;
    if (input.size() > MatterWire::MaxPayload || !StatusJson(input).parse(&fields) || fields["schema"] != "1" ||
        fields.size() != (importing || recreating ? 2U : 1U) || (importing && !hex(fields["dataset"], 2, 508)) ||
        (recreating && fields["confirmation"] != "\"RECREATE\"")) return false;
    *body = importing ? "{\"schema\":1,\"action\":\"import\",\"dataset\":" + fields["dataset"] + '}' :
        recreating ? "{\"schema\":1,\"action\":\"recreate\"}" :
        retrying ? "{\"schema\":1,\"action\":\"retry\"}" :
                   "{\"schema\":1,\"action\":\"create\"}";
    return true;
}
#ifdef MICROHAD_TESTING
bool MatterChannel::threadChannel(const std::string& input, unsigned char channel, std::string* body) {
    body->clear();
    if (channel < 11 || channel > 26 || !requestValid(9, input)) return false;
    Fields fields; if (!StatusJson(input).parse(&fields)) return false;
    if (fields["action"] == "\"retry\"") { *body = input; return true; }
    std::ostringstream out;
    out << "{\"schema\":1,\"action\":" << fields["action"] << ",\"zigbee_channel\":"
        << static_cast<unsigned int>(channel);
    if (fields["action"] == "\"import\"") out << ",\"dataset\":" << fields["dataset"];
    out << '}'; *body = out.str(); return true;
}
#endif
bool MatterChannel::deviceReadRequest(const std::string& input, std::string* body) {
    body->clear();
    Fields fields;
    if(StatusJson(input).parse(&fields) && fields.size()==5 && fields["cluster"]=="513") {
        *body="{\"action\":\"read\","+input.substr(1);return requestValid(10,*body);
    }
    fields.clear();
    if (input.size() > MatterWire::MaxPayload || !StatusJson(input).parse(&fields) || fields.size() != 5 || fields["schema"] != "1" ||
        !hex(fields["node"], 16, 16) || fields["node"] <= "\"0000000000000001\"" ||
        !number(fields["endpoint"], 0, 65534) ||
        (fields["cluster"] != "6" && fields["cluster"] != "8" && fields["cluster"] != "69" && fields["cluster"] != "91" &&
         fields["cluster"] != "1026" && fields["cluster"] != "1029" &&
         fields["cluster"] != "1037" && fields["cluster"] != "1066") || fields["attribute"] != "0") return false;
    *body = "{\"schema\":1,\"action\":\"read\",\"node\":" + fields["node"] +
        ",\"endpoint\":" + fields["endpoint"] + ",\"cluster\":" + fields["cluster"] + ",\"attribute\":0}";
    return requestValid(10, *body);
}
bool MatterChannel::deviceCommandRequest(const std::string& input, std::string* body) {
    body->clear();
    Fields fields;
    static const char* const commands[] = {"\"on\"", "\"off\"", "\"toggle\"", "\"sync_time\"", "\"identify\""};
    if (input.size() > MatterWire::MaxPayload || !StatusJson(input).parse(&fields) || fields["schema"] != "1" ||
        !hex(fields["node"], 16, 16) || fields["node"] <= "\"0000000000000001\"" ||
        !number(fields["endpoint"], 0, 65534)) return false;
    if(fields["command"]=="\"thermostat\"") { *body=input; return requestValid(11,*body); }
    const bool level = fields["command"] == "\"move_to_level\"";
    const bool syncTime = fields["command"] == "\"sync_time\"";
    if ((level && (fields.size() != 6 || !number(fields["level"], 0, 254) ||
                   !number(fields["transition_deciseconds"], 0, 65535))) ||
        (syncTime && (fields.size() != 6 || !signedNumber(fields["offset_seconds"], -50400, 50400) || fields["time_zone"].size() < 3)) ||
        (!level && !syncTime && (fields.size() != 4 || !member(fields["command"], commands, 5)))) return false;
    *body = "{\"schema\":1,\"command\":" + fields["command"] + ",\"node\":" + fields["node"] +
        ",\"endpoint\":" + fields["endpoint"];
    if (level) *body += ",\"level\":" + fields["level"] + ",\"transition_deciseconds\":" + fields["transition_deciseconds"];
    if (syncTime) *body += ",\"time_zone\":" + fields["time_zone"] + ",\"offset_seconds\":" + fields["offset_seconds"];
    *body += '}';
    return requestValid(11, *body);
}
bool MatterChannel::deviceSubscriptionsRequest(const std::string& input, std::string* body) {
    body->clear();
    Fields fields;
    if (!StatusJson(input).parse(&fields) || fields.size() != 4 || fields["schema"] != "1") return false;
    *body = "{\"schema\":1,\"action\":\"subscriptions\",\"node\":" + fields["node"] +
        ",\"restore\":" + fields["restore"] + ",\"cursor\":" + fields["cursor"] + "}";
    return requestValid(10, *body);
}

bool MatterChannel::deviceBindingRequest(const std::string& input, std::string* body) {
    body->clear();
    Fields fields;
    if (input.size() > MatterWire::MaxPayload || !StatusJson(input).parse(&fields) || fields.size() != 4 || fields["schema"] != "1" ||
        !hex(fields["node"], 16, 16) || fields["node"] <= "\"0000000000000001\"" ||
        !number(fields["endpoint"], 0, 65534) || fields["targets"].size() < 3 ||
        fields["targets"][0] != '[' || fields["targets"][fields["targets"].size() - 1] != ']') return false;
    *body = "{\"schema\":1,\"action\":\"bind\",\"node\":" + fields["node"] +
        ",\"endpoint\":" + fields["endpoint"] + ",\"targets\":" + fields["targets"] + "}";
    return requestValid(10, *body);
}

#ifdef MICROHAD_TESTING
bool MatterChannel::inventoryNodesEvent(const std::string& input,std::vector<std::string>* nodes){Body b;return MatterWire::parseJson(2,12,input,&b)&&inventoryNodesEvent(b,nodes);}
#endif


#ifdef MICROHAD_TESTING
bool MatterChannel::inventoryEvent(const std::string& input,std::string* kind,unsigned* count,MatterEndpoint* endpoint){Body b;return MatterWire::parseJson(2,12,input,&b)&&inventoryEvent(b,kind,count,endpoint);}
#endif


#ifdef MICROHAD_TESTING
bool MatterChannel::deviceResult(const std::string& input,const std::string& id,MatterReading* reading){Body b;return MatterWire::parseJson(1,10,input,&b)&&deviceResult(b,strtoull(id.c_str(),0,16),reading);}
#endif


#ifdef MICROHAD_TESTING
bool MatterChannel::deviceReportEvent(const std::string& input,MatterReading* reading){Body b;return MatterWire::parseJson(2,12,input,&b)&&deviceReportEvent(b,reading);}
#endif


#ifdef MICROHAD_TESTING
bool MatterChannel::deviceButtonEvent(const std::string& input,MatterButtonEvent* button){Body b;return MatterWire::parseJson(2,12,input,&b)&&deviceButtonEvent(b,button);}
#endif


bool MatterChannel::relayRequest(const std::string& input, uint32_t* id, unsigned char* opcode,
                                std::string* session, std::string* body) {
    *id = 0; *opcode = 0; session->clear(); body->clear();
    Fields fields, request;
    if (input.size() > MatterWire::MaxPayload || !StatusJson(input).parse(&fields) || fields.size() != 4 ||
        fields["schema"] != "1" || !number(fields["id"], 1, 0xffffffffU) || !number(fields["opcode"], 4, 8) ||
        !fields.count("body") || !StatusJson(fields["body"]).parse(&request)) return false;
    unsigned int operation = static_cast<unsigned int>(fields["opcode"][0] - '0');
    if (!requestValid(operation, fields["body"])) return false;
    uint64_t correlation = 0;
    for (unsigned long i = 0; i < fields["id"].size(); ++i) correlation = correlation * 10 + fields["id"][i] - '0';
    *id = static_cast<uint32_t>(correlation); *opcode = static_cast<unsigned char>(operation);
    *session = request["session"].substr(1, 16);
    *body = operation == 4 ? "{\"schema\":1,\"session\":\"" + *session + "\",\"subscribe\":true}" : fields["body"];
    return true;
}
#ifdef MICROHAD_TESTING
uint32_t MatterChannel::relayGeneration(const std::string& input){Body b;return MatterWire::parseJson(1,6,input,&b)?relayGeneration(b):0;}
#endif

#ifdef MICROHAD_TESTING
bool MatterChannel::relayEvent(const std::string& input,const std::string& id,uint32_t generation){Body b;return MatterWire::parseJson(2,12,input,&b)&&relayEvent(b,strtoull(id.c_str(),0,16),generation);}
#endif


#ifdef MICROHAD_TESTING
std::string MatterChannel::relayResponseSession(const std::string& input){Body b;if(!MatterWire::parseJson(1,4,input,&b))return "";const uint64_t id=relayResponseSession(b);return id?MatterWire::hexId(id):"";}
#endif


bool MatterChannel::requestFromJson(unsigned char op,const std::string& input,Body* body){return validRequest(op,input)&&MatterWire::parseJson(0,op,input,body);}
