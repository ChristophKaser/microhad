#include "matter_protocol.h"

namespace MatterWire {
namespace {
const unsigned int HeaderSize = 10;
bool valid(const Frame& frame) {
    return frame.kind <= Frame::Event && frame.opcode >= 1 && frame.opcode <= 12 &&
        frame.id <= 0xffffffffUL && ((frame.kind == Frame::Event) == (frame.id == 0)) &&
        frame.payload.size() <= MaxPayload;
}
unsigned long get32(const Bytes& bytes, unsigned int at) {
    unsigned long value = 0;
    for (unsigned int i = 0; i < 4; ++i) value |= static_cast<unsigned long>(bytes[at + i]) << (i * 8);
    return value;
}
}
bool encode(const Frame& frame, Bytes* bytes) {
    bytes->clear();
    if (!valid(frame)) return false;
    bytes->push_back(static_cast<unsigned char>(frame.payload.size()));
    bytes->push_back(static_cast<unsigned char>(frame.payload.size() >> 8));
    bytes->push_back(1);
    bytes->push_back(frame.kind);
    bytes->push_back(frame.opcode);
    bytes->push_back(0);
    for (unsigned int i = 0; i < 4; ++i) bytes->push_back(static_cast<unsigned char>(frame.id >> (i * 8)));
    bytes->insert(bytes->end(), frame.payload.begin(), frame.payload.end());
    return true;
}
bool Decoder::fail() {
    failed_ = true;
    input_.clear();
    frames_.clear();
    return false;
}
bool Decoder::feed(const Bytes& bytes) {
    if (failed_) return false;
    if (bytes.size() > MaxChunk || input_.size() + bytes.size() > MaxPayload + HeaderSize + MaxChunk) return fail();
    input_.insert(input_.end(), bytes.begin(), bytes.end());
    while (input_.size() >= HeaderSize) {
        const unsigned int size = input_[0] | (static_cast<unsigned int>(input_[1]) << 8);
        Frame frame;
        frame.kind = input_[3];
        frame.opcode = input_[4];
        frame.id = get32(input_, 6);
        if (input_[2] != 1 || input_[5] != 0 || size > MaxPayload || !valid(frame)) return fail();
        if (input_.size() < HeaderSize + size) return true;
        if (frames_.size() >= MaxQueued) return fail();
        frame.payload.assign(input_.begin() + HeaderSize, input_.begin() + HeaderSize + size);
        frames_.push_back(frame);
        input_.erase(input_.begin(), input_.begin() + HeaderSize + size);
    }
    return true;
}
bool Decoder::pop(Frame* frame) {
    if (frames_.empty()) return false;
    *frame = frames_.front();
    frames_.pop_front();
    return true;
}
}
