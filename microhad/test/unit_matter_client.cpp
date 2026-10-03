#include "matter_client.h"
#include "matter_protocol.h"

#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>

namespace {
struct Peer {
    int fd;
    unsigned char opcode;
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
    assert(request.id == 1 && request.payload.empty());
    MatterWire::Frame response;
    response.kind = MatterWire::Frame::Response;
    response.opcode = request.opcode;
    response.id = request.id;
    const char* json = "{\"enabled\":true,\"state\":\"ready\"}";
    response.payload.assign(json, json + strlen(json));
    assert(MatterWire::encode(response, &bytes));
    for (unsigned int i = 0; i < bytes.size(); ++i) assert(write(client, &bytes[i], 1) == 1);
    close(client);
    return 0;
}
}

void runMatterClientUnitTests() {
    Peer peer;
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    peer.fd = sockets[1];
    peer.opcode = 2;
    pthread_t server;
    assert(pthread_create(&server, 0, serveStatus, &peer) == 0);
    MatterClient client("/tmp/microhad-matter-status-test-missing.sock");
    std::string json, error;
    assert(client.requestStatus(sockets[0], true, &json, &error));
    assert(json == "{\"enabled\":true,\"state\":\"ready\"}");
    assert(pthread_join(server, 0) == 0);
    assert(!client.status(false, &json, &error));
    assert(error == "Matter service socket unavailable");
}
