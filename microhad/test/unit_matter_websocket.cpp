#include "matter_websocket.h"
#include <assert.h>

namespace {
std::string masked(unsigned char opcode, const std::string& payload, bool final = true) {
    std::string result;
    result += static_cast<char>((final ? 0x80 : 0) | opcode);
    if (payload.size() < 126) result += static_cast<char>(0x80 | payload.size());
    else {
        result += static_cast<char>(0xfe);
        result += static_cast<char>(payload.size() >> 8);
        result += static_cast<char>(payload.size());
    }
    result += "abcd";
    for (unsigned long i = 0; i < payload.size(); ++i) result += static_cast<char>(payload[i] ^ "abcd"[i%4]);
    return result;
}
void rejected(const std::string& input, unsigned int code) {
    MatterWebSocket::Decoder decoder;
    assert(!decoder.feed(input) && decoder.closeCode() == code);
    MatterWebSocket::Message message; assert(!decoder.pop(&message));
    assert(!decoder.feed(masked(1, "ok")));
}
}
void runMatterWebSocketUnitTests() {
    const std::string prefix = "GET /api/matter/relay HTTP/1.1\r\nHost: localhost\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\n";
    const std::string key = "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    std::string response;
    assert(MatterWebSocket::handshake(prefix+key+"\r\n", "/api/matter/relay", &response));
    assert(response == "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n");
    assert(!MatterWebSocket::handshake(prefix+key+key+"\r\n", "/api/matter/relay", &response) && response.empty());
    assert(!MatterWebSocket::handshake(prefix+"Sec-WebSocket-Key: bad\r\n\r\n", "/api/matter/relay", &response));
    assert(!MatterWebSocket::handshake(prefix+key+"Content-Length: 1\r\n\r\nx", "/api/matter/relay", &response));
    assert(!MatterWebSocket::handshake(prefix+key+"\r\n", "/wrong", &response));
    std::string badVersion = prefix; badVersion.replace(badVersion.find("13"), 2, "12");
    assert(!MatterWebSocket::handshake(badVersion+key+"\r\n", "/api/matter/relay", &response));
    MatterWebSocket::Decoder decoder;
    const std::string text = "{\"schema\":1}";
    const std::string stream = masked(1, text.substr(0, 5), false) + masked(9, "ping") + masked(0, text.substr(5));
    for (unsigned long i = 0; i < stream.size(); ++i) assert(decoder.feed(stream.substr(i, 1)));
    MatterWebSocket::Message message;
    assert(decoder.pop(&message) && message.opcode == 9 && message.payload == "ping");
    assert(decoder.pop(&message) && message.opcode == 1 && message.payload == text);
    assert(!decoder.pop(&message));
    const std::string unicode("\xf0\x9f\x98\x80", 4);
    assert(decoder.feed(masked(1, unicode.substr(0, 2), false)+masked(0, unicode.substr(2))));
    assert(decoder.pop(&message) && message.payload == unicode);
    assert(decoder.feed(masked(1, std::string(2048, 'x'))));
    assert(decoder.pop(&message) && message.payload.size() == 2048);
    assert(MatterWebSocket::encode(message, &response));
    assert(response.size() == 2052 && static_cast<unsigned char>(response[1]) == 126 &&
           static_cast<unsigned char>(response[2]) == 8 && response[3] == 0);
    message.opcode = 10; message.payload = "ping";
    assert(MatterWebSocket::encode(message, &response) && response == std::string("\x8a\x04ping", 6));
    message.payload.assign(126, 'x'); assert(!MatterWebSocket::encode(message, &response));
    rejected(std::string("\x81\x01x", 3), 1002); // Unmasked client frame.
    rejected(masked(0, "x"), 1002);
    rejected(masked(1, "x", false)+masked(1, "y"), 1002);
    rejected(masked(9, "x", false), 1002);
    rejected(masked(2, "binary"), 1003);
    rejected(masked(1, std::string("\xc0\x80", 2)), 1007);
    rejected(masked(1, std::string("\xed\xa0\x80", 3)), 1007);
    rejected(masked(1, std::string(2049, 'x')), 1009);
    rejected(masked(1, std::string(1500, 'x'), false)+masked(0, std::string(600, 'y')), 1009);
    std::string noncanonical = std::string("\x81\xfe\x00\x01", 4)+"abcdx"; rejected(noncanonical, 1002);
    rejected(masked(8, "x"), 1002);
    rejected(masked(8, std::string("\x03\xed", 2)), 1002); // Reserved close code 1005.
    MatterWebSocket::Decoder crowded;
    for (unsigned int i = 0; i < 8; ++i) assert(crowded.feed(masked(1, "x")));
    assert(!crowded.feed(masked(1, "x")) && crowded.closeCode() == 1009);
    MatterWebSocket::Decoder closing;
    assert(closing.feed(masked(8, std::string("\x03\xe8", 2))));
    assert(closing.pop(&message) && message.opcode == 8);
    assert(!closing.feed(masked(1, "late")));
}
