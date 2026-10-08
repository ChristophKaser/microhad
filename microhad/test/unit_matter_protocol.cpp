#include "matter_protocol.h"
#include <cassert>

void runMatterProtocolUnitTests() {
    using namespace MatterWire;
    const unsigned char golden[] = {3, 0, 1, 0, 7, 0, 0x78, 0x56, 0x34, 0x12, 0, 0x80, 0xff};
    Frame frame;
    frame.opcode = 7;
    frame.id = 0x12345678UL;
    frame.payload.assign(golden + 10, golden + 13);
    Bytes bytes;
    assert(encode(frame, &bytes) && bytes == Bytes(golden, golden + 13));
    Decoder fragmented;
    for (unsigned int i = 0; i < bytes.size(); ++i) assert(fragmented.feed(Bytes(1, bytes[i])));
    Frame decoded;
    assert(fragmented.pop(&decoded) && decoded.id == frame.id && decoded.payload == frame.payload);
    assert(!fragmented.pop(&decoded));
    Bytes together = bytes;
    together.insert(together.end(), bytes.begin(), bytes.end());
    Decoder joined;
    assert(joined.feed(together) && joined.pop(&decoded) && joined.pop(&decoded) && !joined.pop(&decoded));
    for (unsigned int at = 0; at < 4; ++at) {
        Bytes malformed = bytes;
        if (at == 0) malformed[2] = 2;
        if (at == 1) malformed[5] = 1;
        if (at == 2) malformed[4] = 13;
        if (at == 3) malformed[3] = Frame::Event;
        Decoder rejected;
        assert(!rejected.feed(malformed) && rejected.failed() && !rejected.feed(bytes));
    }
    Frame maximum;
    maximum.id = 1;
    maximum.payload.assign(MaxPayload, 0xff);
    assert(encode(maximum, &bytes));
    Decoder bounded;
    assert(bounded.feed(bytes) && bounded.pop(&decoded) && decoded.payload.size() == MaxPayload);
    maximum.payload.push_back(0);
    assert(!encode(maximum, &bytes) && bytes.empty());
    Decoder tooLarge;
    assert(!tooLarge.feed(Bytes(MaxChunk + 1, 0)));
    Frame event;
    event.kind = Frame::Event;
    event.opcode = 12;
    assert(encode(event, &bytes));
    Decoder queue;
    for (unsigned int i = 0; i < MaxQueued; ++i) assert(queue.feed(bytes));
    assert(!queue.feed(bytes) && !queue.pop(&decoded));
}
