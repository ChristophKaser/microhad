#include "ash.h"

namespace {
const unsigned char kFlag = 0x7e;
const unsigned char kEscape = 0x7d;
const unsigned char kStuff = 0x20;
const unsigned char kCancel = 0x1a;

bool reserved(unsigned char value) {
    return value == 0x7e || value == 0x7d || value == 0x11 || value == 0x13 ||
           value == 0x18 || value == 0x1a;
}
}

AshTransport::AshTransport() {
}

unsigned short AshTransport::crc16(const std::vector<unsigned char>& data) {
    unsigned short crc = 0xffff;
    unsigned long i;
    for (i = 0; i < data.size(); ++i) {
        int bit;
        crc ^= static_cast<unsigned short>(data[i]) << 8;
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<unsigned short>((crc << 1) ^ 0x1021) :
                                   static_cast<unsigned short>(crc << 1);
        }
    }
    return crc;
}

std::vector<unsigned char> AshTransport::encodeData(unsigned char control,
                                                      const std::vector<unsigned char>& payload) {
    std::vector<unsigned char> raw;
    std::vector<unsigned char> output;
    unsigned short crc;
    unsigned long i;
    raw.push_back(control);
    raw.insert(raw.end(), payload.begin(), payload.end());
    crc = crc16(raw);
    raw.push_back(static_cast<unsigned char>(crc >> 8));
    raw.push_back(static_cast<unsigned char>(crc & 0xff));
    for (i = 0; i < raw.size(); ++i) {
        if (reserved(raw[i])) {
            output.push_back(kEscape);
            output.push_back(raw[i] ^ kStuff);
        } else {
            output.push_back(raw[i]);
        }
    }
    output.push_back(kFlag);
    return output;
}

void AshTransport::feed(const unsigned char* bytes, unsigned long length) {
    unsigned long i;
    for (i = 0; i < length; ++i) {
        if (bytes[i] == kCancel) {
            input_.clear();
        } else {
            input_.push_back(bytes[i]);
            if (bytes[i] == kFlag) parse();
        }
    }
    parse();
}

bool AshTransport::popFrame(std::vector<unsigned char>* frame) {
    if (frames_.empty()) {
        return false;
    }
    *frame = frames_.front();
    frames_.erase(frames_.begin());
    return true;
}

void AshTransport::reset() {
    input_.clear();
    frames_.clear();
}

void AshTransport::parse() {
    std::vector<unsigned char>::iterator flag;
    while ((flag = input_.begin()) != input_.end()) {
        std::vector<unsigned char> packet;
        std::vector<unsigned char> decoded;
        bool escaped = false;
        unsigned long i;
        flag = input_.begin();
        while (flag != input_.end() && *flag != kFlag) {
            ++flag;
        }
        if (flag == input_.end()) {
            return;
        }
        packet.assign(input_.begin(), flag);
        input_.erase(input_.begin(), flag + 1);
        for (i = 0; i < packet.size(); ++i) {
            if (escaped) {
                decoded.push_back(packet[i] ^ kStuff);
                escaped = false;
            } else if (packet[i] == kEscape) {
                escaped = true;
            } else {
                decoded.push_back(packet[i]);
            }
        }
        if (!escaped && decoded.size() >= 3) {
            unsigned short received = static_cast<unsigned short>(decoded[decoded.size() - 2]) << 8;
            received |= decoded[decoded.size() - 1];
            decoded.resize(decoded.size() - 2);
            if (crc16(decoded) == received) {
                frames_.push_back(decoded);
            }
        }
    }
}
