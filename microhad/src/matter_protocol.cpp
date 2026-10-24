#include "matter_protocol.h"

namespace MatterWire {
bool encodeProtobuf(const Frame&, Bytes*);
bool decodeProtobuf(const Bytes&, Frame*);
namespace {
const unsigned int HeaderSize = 2;
bool valid(const Frame& frame) {
    return frame.kind <= Frame::Event && frame.opcode >= 1 && frame.opcode <= 12 &&
        frame.id <= 0xffffffffUL && ((frame.kind == Frame::Event) == (frame.id == 0)) &&
        ((frame.kind == Frame::Event) == (frame.opcode == 12));
}

}
bool encode(const Frame& frame, Bytes* bytes) {
    bytes->clear();
    if (!valid(frame)) return false;
    return encodeProtobuf(frame, bytes);
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
        if (size == 0 || size > MaxPayload) return fail();
        if (input_.size() < HeaderSize + size) return true;
        if (frames_.size() >= MaxQueued) return fail();
        if (!decodeProtobuf(Bytes(input_.begin() + HeaderSize, input_.begin() + HeaderSize + size), &frame)) return fail();
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
