#include "matter_websocket.h"
#include <map>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <stdint.h>

namespace MatterWebSocket {
namespace {
std::string lower(std::string value) {
    for (unsigned long i = 0; i < value.size(); ++i)
        if (value[i] >= 'A' && value[i] <= 'Z') value[i] += 'a' - 'A';
    return value;
}
std::string trim(std::string value) {
    while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value.erase(0, 1);
    while (!value.empty() && (value[value.size()-1] == ' ' || value[value.size()-1] == '\t')) value.erase(value.size()-1);
    return value;
}
bool token(const std::string& value, const char* wanted) {
    unsigned long at = 0;
    while (at < value.size()) {
        std::string::size_type end = value.find(',', at);
        if (lower(trim(value.substr(at, end == std::string::npos ? end : end-at))) == wanted) return true;
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return false;
}
bool utf8(const std::string& value) {
    unsigned long at = 0;
    while (at < value.size()) {
        unsigned char first = value[at++];
        if (first < 128) continue;
        unsigned int count; uint32_t code, minimum;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 31; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 15; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 7; minimum = 0x10000; }
        else return false;
        if (value.size() - at < count) return false;
        for (unsigned int i = 0; i < count; ++i) {
            unsigned char next = value[at++];
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
bool closePayload(const std::string& payload) {
    if (payload.empty()) return true;
    if (payload.size() == 1) return false;
    unsigned int code = (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]);
    return ((code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006) ||
            (code >= 3000 && code <= 4999)) && utf8(payload.substr(2));
}
}
bool handshake(const std::string& request, const std::string& path, std::string* response) {
    response->clear();
    if (request.size() > 8192 || request.find("GET " + path + " HTTP/1.1\r\n") != 0) return false;
    const std::string::size_type end = request.find("\r\n\r\n");
    if (end == std::string::npos || end + 4 != request.size()) return false;
    std::map<std::string, std::string> fields;
    std::string::size_type at = request.find("\r\n") + 2;
    while (at < end) {
        std::string::size_type lineEnd = request.find("\r\n", at), colon = request.find(':', at);
        if (colon == std::string::npos || colon >= lineEnd || colon == at) return false;
        std::string name = lower(request.substr(at, colon-at));
        for (unsigned long i = 0; i < name.size(); ++i)
            if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '-')) return false;
        std::string value = trim(request.substr(colon+1, lineEnd-colon-1));
        for (unsigned long i = 0; i < value.size(); ++i)
            if (static_cast<unsigned char>(value[i]) < 32 || static_cast<unsigned char>(value[i]) > 126) return false;
        if (fields.count(name)) {
            if (name != "connection" && name != "upgrade") return false;
            fields[name] += ',' + value;
        } else fields[name] = value;
        if (fields.size() > 32) return false;
        at = lineEnd + 2;
    }
    if (fields["host"].empty() || !token(fields["connection"], "upgrade") || !token(fields["upgrade"], "websocket") ||
        fields["sec-websocket-version"] != "13" || fields.count("transfer-encoding") ||
        (fields.count("content-length") && fields["content-length"] != "0")) return false;
    const std::string key = fields["sec-websocket-key"];
    unsigned char decoded[32], encoded[64], digest[20]; size_t length = 0;
    if (key.size() != 24 || mbedtls_base64_decode(decoded, sizeof(decoded), &length,
        reinterpret_cast<const unsigned char*>(key.data()), key.size()) != 0 || length != 16) return false;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &length, decoded, 16) != 0 ||
        std::string(reinterpret_cast<char*>(encoded), length) != key) return false;
    const std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    if (mbedtls_sha1_ret(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest) != 0 ||
        mbedtls_base64_encode(encoded, sizeof(encoded), &length, digest, sizeof(digest)) != 0) return false;
    *response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
        std::string(reinterpret_cast<char*>(encoded), length) + "\r\n\r\n";
    return true;
}
bool encode(const Message& message, std::string* frame) {
    frame->clear();
    const unsigned int opcode = message.opcode;
    const unsigned long size = message.payload.size();
    if ((opcode != 1 && opcode != 8 && opcode != 9 && opcode != 10) || size > MaxMessage ||
        (opcode >= 8 && size > 125) || (opcode == 1 && !utf8(message.payload)) ||
        (opcode == 8 && !closePayload(message.payload))) return false;
    *frame += static_cast<char>(0x80 | opcode);
    if (size < 126) *frame += static_cast<char>(size);
    else { *frame += static_cast<char>(126); *frame += static_cast<char>(size >> 8); *frame += static_cast<char>(size); }
    *frame += message.payload; return true;
}
bool Decoder::fail(unsigned int code) {
    closeCode_ = code; input_.clear(); fragments_.clear(); messages_.clear(); return false;
}
bool Decoder::feed(const std::string& bytes) {
    if (closeCode_ != 0) return false;
    if (closed_) return bytes.empty() || fail(1002);
    if (bytes.size() > MaxChunk || input_.size() + bytes.size() > MaxMessage + 14 + MaxChunk) return fail(1009);
    input_ += bytes;
    while (input_.size() >= 2) {
        const unsigned char first = input_[0], second = input_[1];
        const bool final = (first & 0x80) != 0;
        unsigned char opcode = first & 15;
        unsigned long size = second & 127, offset = 2;
        if ((first & 0x70) || !(second & 0x80) ||
            (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10) ||
            (opcode >= 8 && (!final || size >= 126))) return fail(1002);
        if (opcode == 2) return fail(1003);
        if (size == 127) return fail(1009);
        if (size == 126) {
            if (input_.size() < 4) return true;
            size = (static_cast<unsigned char>(input_[2]) << 8) | static_cast<unsigned char>(input_[3]);
            offset = 4;
            if (size < 126) return fail(1002);
        }
        if (size > MaxMessage || (opcode <= 1 && fragments_.size() + size > MaxMessage)) return fail(1009);
        if (input_.size() < offset + 4 + size) return true;
        std::string payload;
        for (unsigned long i = 0; i < size; ++i) payload += static_cast<char>(
            static_cast<unsigned char>(input_[offset+4+i]) ^ static_cast<unsigned char>(input_[offset+(i%4)]));
        input_.erase(0, offset + 4 + size);
        if (opcode == 0 || opcode == 1) {
            if ((opcode == 0) != fragmented_) return fail(1002);
            fragments_ += payload;
            if (!final) { fragmented_ = true; continue; }
            payload.swap(fragments_); fragments_.clear(); fragmented_ = false; opcode = 1;
            if (!utf8(payload)) return fail(1007);
        }
        if (opcode == 8) {
            if (payload.size() >= 2 && !utf8(payload.substr(2))) return fail(1007);
            if (!closePayload(payload)) return fail(1002);
            if (!input_.empty()) return fail(1002);
            closed_ = true; fragments_.clear(); fragmented_ = false;
        }
        if (messages_.size() >= MaxQueued) return fail(1009);
        Message message; message.opcode = opcode; message.payload.swap(payload); messages_.push_back(message);
    }
    return true;
}
bool Decoder::pop(Message* message) {
    if (messages_.empty()) return false;
    *message = messages_.front(); messages_.pop_front(); return true;
}
}
