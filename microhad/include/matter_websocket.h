#ifndef MICROHAD_MATTER_WEBSOCKET_H
#define MICROHAD_MATTER_WEBSOCKET_H

#include <deque>
#include <string>

// Bounded RFC 6455 server transport; application JSON is validated separately.
namespace MatterWebSocket {
const unsigned int MaxMessage = 2048;
const unsigned int MaxChunk = 4096;
const unsigned int MaxQueued = 8;
struct Message {
    unsigned char opcode;
    std::string payload;
    Message() : opcode(1) {}
};
bool handshake(const std::string& request, const std::string& path, std::string* response);
bool encode(const Message& message, std::string* frame);
class Decoder {
public:
    Decoder() : fragmented_(false), closeCode_(0), closed_(false) {}
    bool feed(const std::string& bytes);
    bool pop(Message* message);
    unsigned int closeCode() const { return closeCode_; }
private:
    std::string input_;
    std::string fragments_;
    std::deque<Message> messages_;
    bool fragmented_;
    unsigned int closeCode_;
    bool closed_;
    bool fail(unsigned int code);
};
}
#endif
