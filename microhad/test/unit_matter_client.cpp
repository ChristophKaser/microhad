#include "matter_client.h"
#include "matter_protocol.h"

#include <assert.h>
#include <pthread.h>
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
    assert(request.id == 1);
    assert(std::string(request.payload.begin(), request.payload.end()) == peer->body);
    MatterWire::Frame response;
    response.kind = MatterWire::Frame::Response;
    response.opcode = request.opcode;
    response.id = request.id;
    response.payload.assign(peer->reply.begin(), peer->reply.end());
    assert(MatterWire::encode(response, &bytes));
    for (unsigned int i = 0; i < bytes.size(); ++i) assert(write(client, &bytes[i], 1) == 1);
    close(client);
    return 0;
}
}

void runMatterClientUnitTests() {
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
    close(queued); close(listener); assert(unlink(path.c_str()) == 0); assert(rmdir(directory) == 0);
}
