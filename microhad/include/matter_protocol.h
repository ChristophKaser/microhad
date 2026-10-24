#ifndef MICROHAD_MATTER_PROTOCOL_H
#define MICROHAD_MATTER_PROTOCOL_H

#include <deque>
#include <vector>

// Private Unix-stream IPC, protobuf version 3. No packet contents are logged here.
namespace MatterWire {
typedef std::vector<unsigned char> Bytes;
const unsigned int MaxPayload = 2048;
const unsigned int MaxChunk = 4096;
const unsigned int MaxQueued = 32;
enum Opcode { MatterStatus = 1, ThreadStatus = 2 };
struct Frame {
    enum Kind { Request = 0, Response = 1, Event = 2 };
    unsigned char kind;
    unsigned char opcode;
    unsigned long id;
    Bytes payload;
    Frame() : kind(Request), opcode(1), id(0) {}
};
bool encode(const Frame& frame, Bytes* bytes);
class Decoder {
public:
    Decoder() : failed_(false) {}
    bool feed(const Bytes& bytes);
    bool pop(Frame* frame);
    bool failed() const { return failed_; }
private:
    bool failed_;
    Bytes input_;
    std::deque<Frame> frames_;
    bool fail();
};
}
#endif
