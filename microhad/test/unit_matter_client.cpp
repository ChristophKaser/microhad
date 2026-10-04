#include "matter_client.h"
#include "matter_protocol.h"
#include "matter_worker.h"

#include <assert.h>
#include <pthread.h>
#include <sstream>
#include <string.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <vector>

namespace {
struct Peer {
    int fd;
    unsigned char opcode;
    std::string body;
    std::string reply;
    unsigned int id;
    bool keep;
    Peer() : fd(-1), opcode(0), id(1), keep(false) {}
};

bool readExact(int fd, unsigned char* bytes, unsigned int size) {
    unsigned int offset = 0;
    while (offset < size) {
        ssize_t count = read(fd, bytes + offset, size - offset);
        if (count <= 0) return false;
        offset += static_cast<unsigned int>(count);
    }
    return true;
}

void* serveStatus(void* opaque) {
    Peer* peer = static_cast<Peer*>(opaque);
    int client = peer->fd;
    unsigned char header[10];
    assert(readExact(client, header, sizeof(header)));
    MatterWire::Bytes bytes(header, header + sizeof(header));
    const unsigned int payloadSize = header[0] | (static_cast<unsigned int>(header[1]) << 8);
    bytes.resize(sizeof(header) + payloadSize);
    if (payloadSize != 0) assert(readExact(client, &bytes[sizeof(header)], payloadSize));
    MatterWire::Decoder decoder;
    assert(decoder.feed(bytes));
    MatterWire::Frame request;
    assert(decoder.pop(&request));
    assert(request.kind == MatterWire::Frame::Request);
    assert(request.opcode == peer->opcode);
    assert(request.id == peer->id);
    assert(std::string(request.payload.begin(), request.payload.end()) == peer->body);
    MatterWire::Frame response;
    response.kind = MatterWire::Frame::Response;
    response.opcode = request.opcode;
    response.id = request.id;
    response.payload.assign(peer->reply.begin(), peer->reply.end());
    assert(MatterWire::encode(response, &bytes));
    for (unsigned int i = 0; i < bytes.size(); ++i) assert(write(client, &bytes[i], 1) == 1);
    if (!peer->keep) close(client);
    return 0;
}

struct ChannelPeer { int fd; std::string event; bool wrongId; };
void* serveChannel(void* opaque) {
    ChannelPeer* peer = static_cast<ChannelPeer*>(opaque);
    for (unsigned int i = 1; i <= (peer->wrongId ? 1U : 2U); ++i) {
        unsigned char header[10]; assert(readExact(peer->fd, header, 10));
        assert(header[0] == 0 && header[1] == 0 && header[6] == i);
        MatterWire::Frame frame;
        frame.kind = MatterWire::Frame::Event; frame.opcode = 12;
        frame.payload.assign(peer->event.begin(), peer->event.end());
        MatterWire::Bytes bytes; assert(MatterWire::encode(frame, &bytes));
        for (unsigned int n = 0; n < bytes.size(); ++n) assert(write(peer->fd, &bytes[n], 1) == 1);
        frame.kind = MatterWire::Frame::Response; frame.opcode = 2; frame.id = peer->wrongId ? 77 : i;
        const std::string reply = "{\"enabled\":true,\"available\":true,\"configured\":false,\"attached\":false,\"state\":\"detached\",\"linux_ipv6_or_tun\":false}";
        frame.payload.assign(reply.begin(), reply.end()); assert(MatterWire::encode(frame, &bytes));
        assert(write(peer->fd, &bytes[0], bytes.size()) == static_cast<ssize_t>(bytes.size()));
    }
    close(peer->fd); return 0;
}

struct StalledPeer { int fd; };
void* serveStalledPeer(void* opaque) {
    StalledPeer* peer = static_cast<StalledPeer*>(opaque);
    unsigned char header[10];
    assert(readExact(peer->fd, header, sizeof(header)));
    const unsigned int payloadSize = header[0] | (static_cast<unsigned int>(header[1]) << 8);
    std::vector<unsigned char> body(payloadSize);
    if (payloadSize != 0) assert(readExact(peer->fd, &body[0], payloadSize));
    usleep(150000);
    close(peer->fd);
    return 0;
}

struct WorkerPeer {
    int listener;
    bool fail;
    unsigned int firstId;
    std::string thread;
};
void* serveWorker(void* opaque) {
    WorkerPeer* worker = static_cast<WorkerPeer*>(opaque);
    int fd = accept(worker->listener, 0, 0); assert(fd >= 0);
    for (unsigned int i = 0; i < 4; ++i) {
        Peer peer; peer.fd = fd; peer.id = i + worker->firstId; peer.keep = true;
        peer.opcode = i == 0 ? 9 : i == 3 ? 5 : i;
        if (i == 0) {
            peer.body = "{\"schema\":1,\"action\":\"observe\",\"zigbee_channel\":0,\"zigbee_online\":false}";
            peer.reply = "{\"schema\":1,\"ok\":true,\"result\":" + worker->thread + '}';
        } else if (i == 1) {
            peer.reply = "{\"enabled\":true,\"available\":true,\"controller_initialized\":true,\"fabric_ready\":true,\"state\":\"awaiting_coordinator\"}";
        } else if (i == 2) peer.reply = worker->thread;
        else {
            peer.body = "{\"schema\":1,\"session\":\"0000000000000001\"}";
            peer.reply = "{\"schema\":1,\"ok\":true,\"result\":{\"session\":\"0000000000000001\",\"state\":\"cancelled\",\"deadline_remaining_seconds\":0,\"error\":\"cancelled\"}}";
            usleep(200000); // Queued submission/status reads must not wait for this reply.
        }
        if (i == 3 && worker->fail) {
            unsigned char header[10]; assert(readExact(fd, header, 10));
            assert(header[4] == 5);
            std::vector<unsigned char> body(header[0] | (static_cast<unsigned int>(header[1]) << 8));
            assert(!body.empty() && readExact(fd, &body[0], body.size()));
            usleep(200000); close(fd); return 0;
        }
        serveStatus(&peer);
    }
    MatterWire::Frame event; event.kind = MatterWire::Frame::Event; event.opcode = 12;
    const std::string text = "{\"schema\":1,\"event\":\"thread\",\"data\":" + worker->thread + '}';
    event.payload.assign(text.begin(), text.end()); MatterWire::Bytes bytes;
    assert(MatterWire::encode(event, &bytes));
    assert(write(fd, &bytes[0], bytes.size()) == static_cast<ssize_t>(bytes.size()));
    unsigned char byte; assert(read(fd, &byte, 1) == 0);
    close(fd); return 0;
}
}

void runMatterClientUnitTests() {
    assert(MatterChannel::deviceSnapshotRequest("{\"schema\":1,\"action\":\"snapshot\"}"));
    assert(MatterChannel::deviceSnapshotRequest("{ \"action\" : \"snapshot\", \"schema\" : 1 }"));
    assert(!MatterChannel::deviceSnapshotRequest("{\"schema\":1,\"action\":\"job\",\"job\":\"0000000000000001\"}"));
    const std::string thread = "{\"enabled\":true,\"available\":true,\"configured\":false,\"attached\":false,\"state\":\"detached\",\"linux_ipv6_or_tun\":false}";
    MatterClient client("/tmp/microhad-matter-status-test-missing.sock");
    std::vector<std::string> replies;
    replies.push_back(thread);
    replies.push_back(thread + "garbage");
    replies.push_back("{\"enabled\":true,\"state\":\"detached\"}");
    replies.push_back(thread.substr(0, thread.size()-1) + ",\"dataset\":\"must-not-return\"}");
    replies.push_back(thread.substr(0, thread.size()-1) + ",\"enabled\":true}");
    replies.push_back(thread.substr(0, thread.size()-1) + ",\"zigbee_channel\":27}");
    replies.push_back(thread.substr(0, thread.size()-1) + ",\"zigbee_channel\":\"15\"}");
    replies.push_back(thread.substr(0, thread.size()-1) + ",\"zigbee_channel\":15}");
    replies.push_back(thread.substr(0, thread.size()-1) + ",\"zigbee_channel\":015}");
    replies.push_back("{\"schema\":1,\"ok\":true,\"result\":" + thread + '}');
    replies.push_back("{\"schema\":1,\"ok\":false,\"error\":\"not_ready\"}");
    replies.push_back("{\"schema\":1,\"ok\":false,\"error\":\"secret\"}");
    std::string json, error;
    for (unsigned long i = 0; i < replies.size(); ++i) {
        Peer peer;
        int sockets[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        assert(fcntl(sockets[0], F_SETFL, O_NONBLOCK) == 0);
        peer.fd = sockets[1]; peer.opcode = i < 9 ? 2 : 9; peer.reply = replies[i];
        peer.body = i < 9 ? "" : "{\"schema\":1,\"action\":\"observe\",\"zigbee_channel\":15,\"zigbee_online\":true}";
        pthread_t server;
        assert(pthread_create(&server, 0, serveStatus, &peer) == 0);
        struct timespec now; assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        int64_t deadline = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000 + 1000;
        json = "old";
        bool valid = client.exchange(sockets[0], peer.opcode, peer.body, deadline, &json, &error);
        assert(valid == (i == 0 || i == 7 || i == 9 || i == 10));
        if (valid) assert(json == peer.reply && error.empty());
        else assert(json.empty() && error == "Matter service response invalid");
        assert(pthread_join(server, 0) == 0);
    }

    {
        int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        StalledPeer stalled; stalled.fd = sockets[1];
        pthread_t server; assert(pthread_create(&server, 0, serveStalledPeer, &stalled) == 0);
        struct timespec begin, end;
        assert(clock_gettime(CLOCK_MONOTONIC, &begin) == 0);
        const int64_t deadline = static_cast<int64_t>(begin.tv_sec) * 1000 + begin.tv_nsec / 1000000 + 25;
        json = "old";
        assert(!client.exchange(sockets[0], 2, "", deadline, &json, &error));
        assert(json.empty() && error == "Matter service response timed out");
        assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
        assert((end.tv_sec-begin.tv_sec)*1000 + (end.tv_nsec-begin.tv_nsec)/1000000 < 125);
        assert(pthread_join(server, 0) == 0);
    }

    const std::string pageBody = "{\"schema\":1,\"action\":\"snapshot\"}";
    assert(MatterChannel::validRequest(10, pageBody));
    assert(!MatterChannel::validRequest(10, "{\"schema\":1,\"action\":\"snapshot\",\"cursor\":1}"));
    assert(!MatterChannel::validRequest(10, "{\"schema\":1,\"action\":\"read\"}"));
    const std::string pageReply = "{\"schema\":1,\"ok\":true,\"result\":{\"revision\":\"0000000000000001\","
        "\"devices\":[{\"node\":\"0000000000000002\",\"state\":\"complete\",\"online\":true,"
        "\"id\":\"matter:0000000000000001:0000000000000002:0001\",\"endpoint\":1,"
        "\"servers\":[6,8,29,1026]}],\"next_cursor\":null}}";
    std::string pageRevision;
    long pageCursor = 0;
    std::vector<MatterEndpoint> pageDevices;
    assert(MatterChannel::devicePage(pageReply, &pageRevision, &pageCursor, &pageDevices));
    assert(pageRevision == "0000000000000001" && pageCursor == -1 && pageDevices.size() == 1);
    assert(pageDevices[0].id == "matter:0000000000000001:0000000000000002:0001");
    assert(pageDevices[0].node == "0000000000000002" && pageDevices[0].endpoint == 1 && pageDevices[0].online);
    assert(pageDevices[0].servers.size() == 4 && pageDevices[0].servers[0] == 6 && pageDevices[0].servers[3] == 1026);
    assert(!MatterChannel::devicePage("{\"schema\":1,\"ok\":true,\"result\":{\"job\":\"0000000000000001\",\"state\":\"queued\"}}",
                                      &pageRevision, &pageCursor, &pageDevices));
    const std::string readBody = "{\"schema\":1,\"action\":\"read\",\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":6,\"attribute\":0}";
    const std::string jobBody = "{\"schema\":1,\"action\":\"job\",\"job\":\"0000000000000001\"}";
    const std::string commandBody = "{\"schema\":1,\"command\":\"toggle\",\"node\":\"0000000000000002\",\"endpoint\":1}";
    assert(MatterChannel::validRequest(10, readBody));
    const std::string levelReadBody = "{\"schema\":1,\"action\":\"read\",\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":8,\"attribute\":0}";
    std::string devicePrepared;
    const std::string levelCommandBody = "{\"schema\":1,\"command\":\"move_to_level\",\"node\":\"0000000000000002\",\"endpoint\":1,\"level\":124,\"transition_deciseconds\":20}";
    assert(MatterChannel::validRequest(10, levelReadBody));
    const unsigned int sensorClusters[] = {91, 1026, 1029, 1037, 1066};
    for (unsigned int i = 0; i < sizeof(sensorClusters) / sizeof(sensorClusters[0]); ++i) {
        std::ostringstream sensorRead;
        sensorRead << "{\"schema\":1,\"action\":\"read\",\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":"
                   << sensorClusters[i] << ",\"attribute\":0}";
        assert(MatterChannel::validRequest(10, sensorRead.str()));
        std::ostringstream sensorInput;
        sensorInput << "{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":"
                    << sensorClusters[i] << ",\"attribute\":0}";
        assert(MatterChannel::deviceReadRequest(sensorInput.str(), &devicePrepared));
    }
    assert(!MatterChannel::validRequest(10, "{\"schema\":1,\"action\":\"read\",\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":1026,\"attribute\":1}"));
    assert(MatterChannel::validRequest(10, jobBody));
    assert(MatterChannel::validRequest(11, commandBody));
    assert(MatterChannel::validRequest(11, levelCommandBody));
    assert(MatterChannel::deviceReadRequest("{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":6,\"attribute\":0}", &devicePrepared));
    assert(devicePrepared == readBody);
    assert(MatterChannel::deviceReadRequest("{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":8,\"attribute\":0}", &devicePrepared));
    assert(devicePrepared == levelReadBody);
    assert(MatterChannel::deviceCommandRequest("{\"command\":\"toggle\",\"node\":\"0000000000000002\",\"endpoint\":1}", &devicePrepared));
    assert(devicePrepared == commandBody);
    assert(MatterChannel::deviceCommandRequest("{\"command\":\"move_to_level\",\"node\":\"0000000000000002\",\"endpoint\":1,\"level\":124,\"transition_deciseconds\":20}", &devicePrepared));
    assert(devicePrepared == levelCommandBody);
    assert(!MatterChannel::deviceCommandRequest("{\"command\":\"move_to_level\",\"node\":\"0000000000000002\",\"endpoint\":1,\"level\":255,\"transition_deciseconds\":20}", &devicePrepared));
    assert(!MatterChannel::deviceCommandRequest("{\"command\":\"move_to_level\",\"node\":\"0000000000000002\",\"endpoint\":1,\"level\":1,\"transition_deciseconds\":65536}", &devicePrepared));
    assert(!MatterChannel::validRequest(11, "{\"schema\":1,\"command\":\"move_to_level\",\"node\":\"0000000000000002\",\"endpoint\":1,\"level\":255,\"transition_deciseconds\":0}"));
    assert(!MatterChannel::validRequest(11, "{\"schema\":1,\"command\":\"move_to_level\",\"node\":\"0000000000000002\",\"endpoint\":1}"));
    std::vector<std::string> operationReplies;
    operationReplies.push_back("{\"schema\":1,\"ok\":true,\"result\":{\"job\":\"0000000000000001\",\"state\":\"queued\"}}");
    operationReplies.push_back("{\"schema\":1,\"ok\":true,\"result\":{\"state\":\"queued\",\"job\":\"0000000000000001\"}}");
    operationReplies.push_back("{\"schema\":1,\"ok\":true,\"result\":{\"state\":\"running\",\"job\":\"0000000000000001\"}}");
    operationReplies.push_back("{\"schema\":1,\"ok\":true,\"result\":{\"state\":\"complete\",\"job\":\"0000000000000001\",\"result\":{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":6,\"attribute\":0,\"value\":{\"type\":\"bool\",\"value\":true}}}}");
    operationReplies.push_back("{\"schema\":1,\"ok\":true,\"result\":{\"state\":\"failed\",\"job\":\"0000000000000001\",\"error\":\"not_ready\"}}");
    operationReplies.push_back("{\"schema\":1,\"ok\":true,\"result\":{\"state\":\"complete\",\"job\":\"0000000000000001\",\"result\":{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":8,\"attribute\":0,\"value\":{\"type\":\"u8\",\"value\":124}}}}");
    operationReplies.push_back("{\"schema\":1,\"ok\":true,\"result\":{\"state\":\"complete\",\"job\":\"0000000000000001\",\"result\":{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":8,\"attribute\":0,\"value\":{\"type\":\"u8\",\"value\":null}}}}");
    std::string malformedJob = operationReplies[3]; malformedJob.replace(malformedJob.find("\"cluster\":6"), 10, "\"cluster\":8"); operationReplies.push_back(malformedJob);
    std::string malformedLevel = operationReplies[5]; malformedLevel.replace(malformedLevel.find("\"value\":124"), 12, "\"value\":255"); operationReplies.push_back(malformedLevel);
    std::string serviceJob;
    assert(MatterChannel::acceptedOperationJob(11, operationReplies[0], &serviceJob) && serviceJob == "0000000000000001");
    assert(MatterChannel::operationPollRequest(serviceJob) == jobBody);
    bool terminal = true, failed = false; std::string operationError;
    assert(MatterChannel::operationStatus(operationReplies[1], serviceJob, &terminal, &failed, &operationError));
    assert(!terminal && !failed && operationError.empty());
    assert(MatterChannel::operationStatus(operationReplies[3], serviceJob, &terminal, &failed, &operationError));
    assert(terminal && !failed && operationError.empty());
    assert(MatterChannel::operationStatus(operationReplies[4], serviceJob, &terminal, &failed, &operationError));
    assert(terminal && failed && operationError == "not_ready");
    assert(!MatterChannel::acceptedOperationJob(11, operationReplies[5], &serviceJob));
    for (unsigned int i = 0; i < operationReplies.size(); ++i) {
        int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        assert(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
        MatterChannel channel("unused"); channel.fd_ = pair[0];
        Peer peer; peer.fd = pair[1]; peer.opcode = i == 0 ? 11 : 10;
        peer.body = i == 0 ? commandBody : jobBody; peer.reply = operationReplies[i];
        pthread_t server; assert(pthread_create(&server, 0, serveStatus, &peer) == 0);
        assert(channel.call(peer.opcode, peer.body, &json, &error) == (i < 7));
        assert(pthread_join(server, 0) == 0);
    }
    const std::string sensorReplies[] = {
        "{\"schema\":1,\"ok\":true,\"result\":{\"state\":\"complete\",\"job\":\"0000000000000001\",\"result\":{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":1026,\"attribute\":0,\"value\":{\"type\":\"measurement\",\"value\":{\"value\":-5.25,\"unit\":\"celsius\",\"quality\":\"measured\"}}}}}",
        "{\"schema\":1,\"ok\":true,\"result\":{\"state\":\"complete\",\"job\":\"0000000000000001\",\"result\":{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":1029,\"attribute\":0,\"value\":{\"type\":\"measurement\",\"value\":{\"value\":null,\"unit\":\"percent\",\"quality\":\"unknown\"}}}}}",
        "{\"schema\":1,\"ok\":true,\"result\":{\"state\":\"complete\",\"job\":\"0000000000000001\",\"result\":{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":1037,\"attribute\":0,\"value\":{\"type\":\"measurement\",\"value\":{\"value\":758.5,\"unit\":\"ppm\",\"quality\":\"measured\"}}}}}",
        "{\"schema\":1,\"ok\":true,\"result\":{\"state\":\"complete\",\"job\":\"0000000000000001\",\"result\":{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":1066,\"attribute\":0,\"value\":{\"type\":\"measurement\",\"value\":{\"value\":12.75,\"unit\":\"ug/m3\",\"quality\":\"measured\"}}}}}",
        "{\"schema\":1,\"ok\":true,\"result\":{\"state\":\"complete\",\"job\":\"0000000000000001\",\"result\":{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":91,\"attribute\":0,\"value\":{\"type\":\"air_quality\",\"value\":{\"value\":\"moderate\",\"quality\":\"measured\"}}}}}",
        "{\"schema\":1,\"ok\":true,\"result\":{\"state\":\"complete\",\"job\":\"0000000000000001\",\"result\":{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":1026,\"attribute\":0,\"value\":{\"type\":\"measurement\",\"value\":{\"value\":-5.25,\"unit\":\"fahrenheit\",\"quality\":\"measured\"}}}}}",
        "{\"schema\":1,\"ok\":true,\"result\":{\"state\":\"complete\",\"job\":\"0000000000000001\",\"result\":{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":1029,\"attribute\":0,\"value\":{\"type\":\"measurement\",\"value\":{\"value\":null,\"unit\":\"percent\",\"quality\":\"measured\"}}}}}"
    };
    MatterReading decoded;
    assert(MatterChannel::deviceResult(operationReplies[3], "0000000000000001", &decoded));
    assert(decoded.node == "0000000000000002" && decoded.endpoint == 1 && decoded.cluster == 6 &&
           decoded.known && decoded.booleanValue);
    assert(MatterChannel::deviceResult(operationReplies[5], "0000000000000001", &decoded));
    assert(decoded.cluster == 8 && decoded.known && decoded.numberValue == 124);
    assert(MatterChannel::deviceResult(operationReplies[6], "0000000000000001", &decoded));
    assert(decoded.cluster == 8 && !decoded.known);
    assert(!MatterChannel::deviceResult(operationReplies[3], "0000000000000002", &decoded));
    assert(!MatterChannel::deviceResult(operationReplies[4], "0000000000000001", &decoded));
    for (unsigned int i = 0; i < 5; ++i) {
        assert(MatterChannel::deviceResult(sensorReplies[i], "0000000000000001", &decoded));
        assert(decoded.node == "0000000000000002" && decoded.endpoint == 1);
        if (i == 0) assert(decoded.cluster == 1026 && decoded.known && decoded.numberValue == -5.25 && decoded.unit == "celsius");
        if (i == 1) assert(decoded.cluster == 1029 && !decoded.known && decoded.unit == "percent");
        if (i == 2) assert(decoded.cluster == 1037 && decoded.known && decoded.numberValue == 758.5 && decoded.unit == "ppm");
        if (i == 3) assert(decoded.cluster == 1066 && decoded.known && decoded.numberValue == 12.75 && decoded.unit == "ug/m3");
        if (i == 4) assert(decoded.cluster == 91 && decoded.known && decoded.textValue == "moderate");
    }
    assert(!MatterChannel::deviceResult(sensorReplies[5], "0000000000000001", &decoded));
    assert(!MatterChannel::deviceResult(sensorReplies[6], "0000000000000001", &decoded));
    const std::string onReport =
        "{\"schema\":1,\"event\":\"matter_report\",\"data\":{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":6,\"attribute\":0,\"value\":{\"type\":\"bool\",\"value\":true}}}";
    assert(MatterChannel::deviceReportEvent(onReport, &decoded));
    assert(decoded.node == "0000000000000002" && decoded.endpoint == 1 && decoded.cluster == 6 &&
        decoded.known && decoded.booleanValue);
    const std::string levelReport =
        "{\"schema\":1,\"event\":\"matter_report\",\"data\":{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":8,\"attribute\":0,\"value\":{\"type\":\"u8\",\"value\":124}}}";
    assert(MatterChannel::deviceReportEvent(levelReport, &decoded));
    assert(decoded.cluster == 8 && decoded.known && decoded.numberValue == 124);
    const std::string invalidReport =
        "{\"schema\":1,\"event\":\"matter_report\",\"data\":{\"node\":\"0000000000000002\",\"endpoint\":1,\"cluster\":8,\"attribute\":0,\"value\":{\"type\":\"u8\",\"value\":255}}}";
    assert(!MatterChannel::deviceReportEvent(invalidReport, &decoded));
    for (unsigned int i = 0; i < sizeof(sensorReplies) / sizeof(sensorReplies[0]); ++i) {
        int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        assert(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
        MatterChannel channel("unused"); channel.fd_ = pair[0];
        Peer peer; peer.fd = pair[1]; peer.opcode = 10; peer.body = jobBody; peer.reply = sensorReplies[i];
        pthread_t server; assert(pthread_create(&server, 0, serveStatus, &peer) == 0);
        assert(channel.call(10, jobBody, &json, &error) == (i < 5));
        assert(pthread_join(server, 0) == 0);
    }
    const std::string device = "{\"node\":\"0000000000000002\",\"state\":\"complete\",\"online\":true,"
        "\"id\":\"matter:0000000000000001:0000000000000002:0001\",\"endpoint\":1,\"servers\":[6,29]}";
    std::vector<std::string> pages;
    pages.push_back("{\"schema\":1,\"ok\":true,\"result\":{\"revision\":\"0000000000000001\",\"devices\":[" + device + "],\"next_cursor\":null}}");
    pages.push_back("{\"schema\":1,\"ok\":true,\"result\":{\"revision\":\"0000000000000001\",\"devices\":[],\"next_cursor\":null}}");
    pages.push_back("{\"schema\":1,\"ok\":false,\"error\":\"stale_revision\"}");
    std::string bad = pages[0]; bad.replace(bad.find("[6,29]"), 6, "[29,29]"); pages.push_back(bad);
    bad = pages[0]; bad.replace(bad.find("\"endpoint\":1"), 12, "\"endpoint\":2"); pages.push_back(bad);
    bad = pages[0]; bad.replace(bad.find("\"complete\""), 10, "\"recovery_required\""); pages.push_back(bad);
    bad = pages[0]; bad.replace(bad.find("[6,29]"), 6, "[6]"); pages.push_back(bad);
    bad = pages[0]; bad.replace(bad.find("[" + device + "]"), device.size() + 2, "[" + device + "," + device + "]"); pages.push_back(bad);
    for (unsigned int i = 0; i < pages.size(); ++i) {
        int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        assert(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
        MatterChannel channel("unused"); channel.fd_ = pair[0];
        Peer peer; peer.fd = pair[1]; peer.opcode = 10; peer.body = pageBody; peer.reply = pages[i];
        pthread_t server; assert(pthread_create(&server, 0, serveStatus, &peer) == 0);
        assert(channel.call(10, pageBody, &json, &error) == (i < 3));
        assert(pthread_join(server, 0) == 0);
    }

    const std::string writeEvent = "{\"schema\":1,\"event\":\"ble_write\",\"data\":{\"session\":\"0000000000000001\",\"generation\":4294967295,\"write\":3,\"data\":\"000fff\"}}";
    for (unsigned int i = 0; i < 2; ++i) {
        int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        assert(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
        MatterChannel channel("unused"); channel.fd_ = pair[0];
        ChannelPeer peer; peer.fd = pair[1]; peer.event = writeEvent; peer.wrongId = i != 0;
        pthread_t server; assert(pthread_create(&server, 0, serveChannel, &peer) == 0);
        assert(channel.call(2, "", &json, &error) == (i == 0));
        if (i == 0) {
            assert(json == thread && channel.popEvent(&json) && json == writeEvent);
            assert(channel.call(2, "", &json, &error));
            assert(json == thread && channel.popEvent(&json) && json == writeEvent);
        } else assert(json.empty() && channel.fd_ == -1 && !channel.popEvent(&json));
        assert(pthread_join(server, 0) == 0);
    }

    // Rust's exact cancelled-session vector and callback response bounds.
    for (unsigned int i = 0; i < 4; ++i) {
        Peer peer; int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        peer.fd = pair[1]; peer.opcode = i < 2 ? 5 : 6;
        peer.reply = i < 2 ? "{\"schema\":1,\"ok\":true,\"result\":{\"session\":\"0000000000abcdef\",\"state\":\"cancelled\",\"deadline_remaining_seconds\":0,\"error\":\"cancelled\"}}" :
            "{\"schema\":1,\"ok\":true,\"result\":{\"generation\":4294967295}}";
        if (i == 1) peer.reply.replace(peer.reply.find("\"cancelled\""), 11, "\"unknown\"");
        if (i == 3) peer.reply.replace(peer.reply.find("4294967295"), 10, "4294967296");
        pthread_t server; assert(pthread_create(&server, 0, serveStatus, &peer) == 0);
        struct timespec now; assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        const int64_t deadline = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000 + 1000;
        assert(client.exchange(pair[0], peer.opcode, "", deadline, &json, &error) == (i % 2 == 0));
        assert(pthread_join(server, 0) == 0);
    }
    // Idle event ingestion and malformed/overflow rejection share the same validator.
    for (unsigned int i = 0; i < 5; ++i) {
        int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        MatterChannel channel("unused"); channel.fd_ = pair[0];
        MatterWire::Frame frame; frame.kind = MatterWire::Frame::Event; frame.opcode = 12;
        std::string body = writeEvent;
        if (i == 1) body.replace(body.find("4294967295"), 10, "4294967296");
        if (i == 2) body.replace(body.find("000fff"), 6, "00zzff");
        if (i == 3) body.insert(body.size()-1, ",\"pin\":12345678");
        if (i == 4) for (unsigned int n = 0; n < 8; ++n) channel.events_.push_back(writeEvent);
        frame.payload.assign(body.begin(), body.end()); MatterWire::Bytes bytes;
        assert(MatterWire::encode(frame, &bytes));
        assert(write(pair[1], &bytes[0], bytes.size()) == static_cast<ssize_t>(bytes.size()));
        assert(channel.poll(&error) == (i == 0));
        if (i == 0) assert(channel.popEvent(&json) && json == writeEvent);
        else assert(channel.fd_ == -1 && !channel.popEvent(&json));
        close(pair[1]);
    }
    uint32_t relayId; unsigned char relayOpcode; std::string relaySession, relayBody;
    assert(MatterChannel::relayRequest("{\"schema\":1,\"id\":4294967295,\"opcode\":4,\"body\":{\"schema\":1,\"session\":\"0000000000000001\"}}",
        &relayId, &relayOpcode, &relaySession, &relayBody));
    assert(relayId == 0xffffffffU && relayOpcode == 4 && relaySession == "0000000000000001");
    assert(relayBody == "{\"schema\":1,\"session\":\"0000000000000001\",\"subscribe\":true}");
    assert(!MatterChannel::relayRequest("{\"schema\":1,\"id\":1,\"opcode\":3,\"body\":{}}",
        &relayId, &relayOpcode, &relaySession, &relayBody));
    assert(MatterChannel::relayEvent(writeEvent, "0000000000000001", 0xffffffffU));
    assert(!MatterChannel::relayEvent(writeEvent, "0000000000000001", 1));
    assert(!MatterChannel::relayEvent(writeEvent, "0000000000000002", 0xffffffffU));
    std::string prepared, privateBody;
    assert(MatterChannel::threadMutation(false, "{\"schema\":1}", &prepared));
    assert(MatterChannel::validRequest(9, prepared));
    assert(MatterChannel::threadChannel(prepared, 15, &privateBody));
    assert(privateBody == "{\"schema\":1,\"action\":\"create\",\"zigbee_channel\":15}");
    assert(!MatterChannel::validRequest(9, privateBody)); // Caller cannot queue a channel override.
    assert(!MatterChannel::threadMutation(false, "{\"schema\":1,\"zigbee_channel\":20}", &prepared));
    assert(!MatterChannel::threadMutation(false, "{\"schema\":1,\"schema\":1}", &prepared));
    assert(!MatterChannel::threadMutation(true, "{\"schema\":1,\"dataset\":\"zz\"}", &prepared));
    assert(MatterChannel::threadMutation(true, "{\"schema\":1,\"dataset\":\"" + std::string(508, 'a') + "\"}", &prepared));
    assert(MatterChannel::threadChannel(prepared, 26, &privateBody));
    assert(!MatterChannel::threadChannel(prepared, 0, &privateBody));
    assert(!MatterChannel::threadMutation(true, "{\"schema\":1,\"dataset\":\"" + std::string(510, 'a') + "\"}", &prepared));
    assert(!client.status(false, &json, &error));
    assert(error == "Matter service socket unavailable");
    assert(!client.observe(10, true, &json, &error));
    assert(error == "Invalid coordinator observation");
    // A full Unix listen backlog must fail quickly, not wait indefinitely in connect.
    char directory[] = "/tmp/microhad-matter-connect-XXXXXX";
    assert(mkdtemp(directory));
    std::string path = std::string(directory) + "/ipc.sock";
    int listener = socket(AF_UNIX, SOCK_STREAM, 0); assert(listener >= 0);
    struct sockaddr_un address; memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX; strcpy(address.sun_path, path.c_str());
    assert(bind(listener, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0);
    assert(chmod(path.c_str(), 0600) == 0 && listen(listener, 0) == 0);
    int queued = socket(AF_UNIX, SOCK_STREAM, 0); assert(queued >= 0);
    assert(connect(queued, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0);
    struct timespec begin, end; assert(clock_gettime(CLOCK_MONOTONIC, &begin) == 0);
    MatterClient full(path);
    assert(!full.status(false, &json, &error));
    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    assert((end.tv_sec-begin.tv_sec)*1000 + (end.tv_nsec-begin.tv_nsec)/1000000 < 1500);
    assert(json.empty());
    close(queued);
    int pending = accept(listener, 0, 0); assert(pending >= 0); close(pending);
    WorkerPeer fake; fake.listener = listener; fake.thread = thread; fake.fail = false; fake.firstId = 1;
    pthread_t peerThread; assert(pthread_create(&peerThread, 0, serveWorker, &fake) == 0);
    MatterWorker worker(0, path);
    assert(!worker.status(true, &json, &error));
    assert(worker.start(&error));
    assert(!worker.start(&error));
    bool available = false;
    for (unsigned int i = 0; i < 200; ++i) {
        if (worker.status(true, &json, &error)) { available = true; break; }
        usleep(5000);
    }
    assert(available && json == thread);
    uint32_t jobId = 0;
    assert(!worker.submit(3, "{\"schema\":1,\"discriminator\":0,\"pin\":12345678,\"timeout_seconds\":30}", &jobId, &error));
    assert(error == "malformed" && jobId == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &begin) == 0);
    assert(worker.submit(5, "{\"schema\":1,\"session\":\"0000000000000001\"}", &jobId, &error));
    assert(jobId != 0 && worker.job(jobId, &json));
    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    assert((end.tv_sec-begin.tv_sec)*1000 + (end.tv_nsec-begin.tv_nsec)/1000000 < 100);
    // A blocked/absent IPC peer must never block cached status readers.
    assert(clock_gettime(CLOCK_MONOTONIC, &begin) == 0);
    for (unsigned int i = 0; i < 100; ++i) assert(worker.status(false, &json, &error));
    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    assert((end.tv_sec-begin.tv_sec)*1000 + (end.tv_nsec-begin.tv_nsec)/1000000 < 100);
    bool completed = false, eventReceived = false;
    for (unsigned int i = 0; i < 100; ++i) {
        assert(worker.job(jobId, &json));
        if (json.find("\"state\":\"complete\"") != std::string::npos) completed = true;
        if (worker.takeEvent(&json)) { assert(json.find("\"event\":\"thread\"") != std::string::npos); eventReceived = true; }
        if (completed && eventReceived) break;
        usleep(5000);
    }
    assert(completed && eventReceived);
    worker.stop(); worker.stop();
    assert(pthread_join(peerThread, 0) == 0);
    assert(!worker.job(jobId, &json));
    assert(!worker.status(true, &json, &error));
    fake.fail = true; fake.firstId = 5;
    assert(pthread_create(&peerThread, 0, serveWorker, &fake) == 0);
    assert(worker.start(&error));
    available = false;
    for (unsigned int i = 0; i < 200; ++i) {
        if (worker.status(true, &json, &error)) { available = true; break; }
        usleep(5000);
    }
    assert(available);
    const std::string cancel = "{\"schema\":1,\"session\":\"0000000000000001\"}";
    std::vector<uint32_t> failedJobs;
    assert(worker.submit(5, cancel, &jobId, &error)); failedJobs.push_back(jobId);
    usleep(50000); // First call is blocked at the peer; fill all pending slots.
    for (unsigned int i = 0; i < 8; ++i) {
        assert(worker.submit(5, cancel, &jobId, &error)); failedJobs.push_back(jobId);
    }
    assert(!worker.submit(5, cancel, &jobId, &error) && error == "capacity");
    bool allFailed = false;
    for (unsigned int i = 0; i < 100; ++i) {
        allFailed = true;
        for (unsigned int n = 0; n < failedJobs.size(); ++n) {
            assert(worker.job(failedJobs[n], &json));
            if (json.find("\"state\":\"failed\"") == std::string::npos) allFailed = false;
            assert(json.find("\"session\"") == std::string::npos);
        }
        if (allFailed) break;
        usleep(5000);
    }
    assert(allFailed && !worker.status(true, &json, &error));
    worker.stop(); assert(pthread_join(peerThread, 0) == 0);
    close(listener); assert(unlink(path.c_str()) == 0); assert(rmdir(directory) == 0);
    assert(worker.start(&error));
    assert(!worker.submit(5, "{\"schema\":1,\"session\":\"0000000000000001\"}", &jobId, &error));
    assert(error == "not_ready");
    usleep(60000); assert(!worker.status(false, &json, &error)); worker.stop();
}
