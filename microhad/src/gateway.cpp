#include "gateway.h"
#include "storage.h"
#include "zcl.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sstream>
#include <termios.h>
#include <unistd.h>

Device::Device() : kind("generic"), contactOpen(false), hasContact(false), hasTemperature(false),
                   temperatureCelsius(0), hasBattery(false), batteryPercent(0), linkQuality(0), lastSeen(0) {
}

namespace {
std::vector<unsigned char> randomize(const std::vector<unsigned char>& input) {
    std::vector<unsigned char> output;
    unsigned char random = 0x42;
    unsigned long i;
    for (i = 0; i < input.size(); ++i) {
        output.push_back(input[i] ^ random);
        random = (random & 1) ? static_cast<unsigned char>((random >> 1) ^ 0xb8) :
                                static_cast<unsigned char>(random >> 1);
    }
    return output;
}
}

Gateway::Gateway(const std::string& serialDevice, int serialBaud, const std::string& runtimeDirectory) :
    serialDevice_(serialDevice), serialBaud_(serialBaud), runtimeDirectory_(runtimeDirectory), serialFd_(-1),
    online_(false), joinUntil_(0), ashSendSequence_(0), ashReceiveSequence_(0), ezspSequence_(0),
    ezspVersion_(4), resetDeadline_(0) {
}

Gateway::~Gateway() {
    stop();
}

bool Gateway::configureSerial(std::string* error) {
    struct termios options;
    speed_t speed;
    if (serialBaud_ != 115200) {
        *error = "only the built-in NCP baud rate 115200 is supported";
        return false;
    }
    serialFd_ = open(serialDevice_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serialFd_ < 0) {
        *error = std::string("cannot open Zigbee serial device: ") + strerror(errno);
        return false;
    }
    if (tcgetattr(serialFd_, &options) != 0) {
        *error = std::string("cannot read Zigbee serial settings: ") + strerror(errno);
        close(serialFd_);
        serialFd_ = -1;
        return false;
    }
    speed = B115200;
    cfsetispeed(&options, speed);
    cfsetospeed(&options, speed);
    options.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    options.c_cflag |= CS8 | CLOCAL | CREAD | CRTSCTS;
    options.c_iflag &= ~(IGNBRK | BRKINT | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
    options.c_oflag &= ~OPOST;
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;
    if (tcsetattr(serialFd_, TCSANOW, &options) != 0) {
        *error = std::string("cannot configure Zigbee serial device: ") + strerror(errno);
        close(serialFd_);
        serialFd_ = -1;
        return false;
    }
    return true;
}

bool Gateway::start(std::string* error) {
    if (!ensureDirectory(runtimeDirectory_, error)) {
        return false;
    }
    loadState();
    if (!configureSerial(error)) {
        return false;
    }
    sendAshReset();
    return true;
}

void Gateway::stop() {
    if (serialFd_ >= 0) {
        close(serialFd_);
        serialFd_ = -1;
    }
    online_ = false;
}

void Gateway::poll() {
    unsigned char bytes[256];
    ssize_t count;
    std::vector<unsigned char> frame;
    if (serialFd_ >= 0) {
        count = read(serialFd_, bytes, sizeof(bytes));
        if (count > 0) {
            ash_.feed(bytes, static_cast<unsigned long>(count));
            while (ash_.popFrame(&frame)) handleAshFrame(frame);
        }
    }
    if (!online_ && resetDeadline_ != 0 && time(0) >= resetDeadline_) sendAshReset();
    if (joinUntil_ != 0 && time(0) >= joinUntil_) {
        joinUntil_ = 0;
    }
}

bool Gateway::isOnline() const {
    return online_;
}

bool Gateway::setJoining(unsigned int seconds, std::string* error) {
    if (!online_) {
        *error = "Zigbee coordinator is offline";
        return false;
    }
    if (seconds > 3600) {
        *error = "joining window must not exceed one hour";
        return false;
    }
    std::vector<unsigned char> payload;
    payload.push_back(static_cast<unsigned char>(seconds));
    sendEzspCommand(0x0022, payload);
    joinUntil_ = seconds == 0 ? 0 : time(0) + seconds;
    return true;
}

bool Gateway::renameDevice(const std::string& ieeeAddress, const std::string& name, std::string* error) {
    std::map<std::string, Device>::iterator it = devices_.find(ieeeAddress);
    if (it == devices_.end()) {
        *error = "device not found";
        return false;
    }
    if (name.empty() || name.size() > 64) {
        *error = "device name must be between 1 and 64 characters";
        return false;
    }
    it->second.name = name;
    saveState();
    return true;
}

bool Gateway::applyAttributeReport(const std::string& ieeeAddress, unsigned short cluster, unsigned short attribute,
                                   const std::vector<unsigned char>& value) {
    Device& device = devices_[ieeeAddress];
    bool changed;
    if (device.ieeeAddress.empty()) device.ieeeAddress = ieeeAddress;
    changed = applyZclAttribute(&device, cluster, attribute, value, time(0));
    if (changed) saveState();
    return changed;
}

bool Gateway::applyIasZoneStatus(const std::string& ieeeAddress, const std::vector<unsigned char>& payload) {
    Device& device = devices_[ieeeAddress];
    bool changed;
    if (device.ieeeAddress.empty()) device.ieeeAddress = ieeeAddress;
    changed = ::applyIasZoneStatus(&device, payload, time(0));
    if (changed) saveState();
    return changed;
}

std::vector<Device> Gateway::devices() const {
    std::vector<Device> result;
    std::map<std::string, Device>::const_iterator it;
    for (it = devices_.begin(); it != devices_.end(); ++it) {
        result.push_back(it->second);
    }
    return result;
}

std::string Gateway::statusJson() const {
    std::ostringstream json;
    json << "{\"online\":" << (online_ ? "true" : "false") << ",\"joining_until\":" << joinUntil_
         << ",\"devices\":[";
    std::map<std::string, Device>::const_iterator it;
    bool first = true;
    for (it = devices_.begin(); it != devices_.end(); ++it) {
        const Device& device = it->second;
        if (!first) json << ',';
        first = false;
        json << "{\"ieee_address\":\"" << jsonEscape(device.ieeeAddress) << "\",\"name\":\""
             << jsonEscape(device.name) << "\",\"kind\":\"" << jsonEscape(device.kind)
             << "\",\"contact_open\":" << (device.contactOpen ? "true" : "false")
             << ",\"temperature_celsius\":" << device.temperatureCelsius << ",\"last_seen\":"
             << device.lastSeen << "}";
    }
    json << "]}";
    return json.str();
}

void Gateway::loadState() {
    std::vector<Device> saved;
    std::string error;
    unsigned long i;
    if (loadDevices(runtimeDirectory_ + "/devices.json", &saved, &error)) {
        for (i = 0; i < saved.size(); ++i) devices_[saved[i].ieeeAddress] = saved[i];
    }
}

void Gateway::saveState() const {
    std::vector<Device> saved = devices();
    std::string ignored;
    saveDevices(runtimeDirectory_ + "/devices.json", saved, &ignored);
}

bool Gateway::writeBytes(const std::vector<unsigned char>& bytes) {
    unsigned long offset = 0;
    while (serialFd_ >= 0 && offset < bytes.size()) {
        ssize_t count = write(serialFd_, &bytes[offset], bytes.size() - offset);
        if (count > 0) offset += static_cast<unsigned long>(count);
        else if (count < 0 && errno != EAGAIN && errno != EINTR) return false;
        else break;
    }
    return offset == bytes.size();
}

void Gateway::sendAshReset() {
    std::vector<unsigned char> raw;
    std::vector<unsigned char> reset = AshTransport::encodeData(0xc0, raw);
    reset.insert(reset.begin(), 0x1a);
    ash_.reset();
    online_ = false;
    writeBytes(reset);
    resetDeadline_ = time(0) + 5;
}

void Gateway::sendEzspCommand(unsigned short command, const std::vector<unsigned char>& payload) {
    std::vector<unsigned char> ezsp;
    std::vector<unsigned char> encoded;
    unsigned char control;
    ezsp.push_back(ezspSequence_++);
    if (ezspVersion_ >= 8) {
        ezsp.push_back(0x00); ezsp.push_back(0x01);
        ezsp.push_back(static_cast<unsigned char>(command & 0xff));
        ezsp.push_back(static_cast<unsigned char>(command >> 8));
    } else if (ezspVersion_ >= 5) {
        ezsp.push_back(0x00); ezsp.push_back(0xff); ezsp.push_back(0x00);
        ezsp.push_back(static_cast<unsigned char>(command));
    } else {
        ezsp.push_back(0x00); ezsp.push_back(static_cast<unsigned char>(command));
    }
    ezsp.insert(ezsp.end(), payload.begin(), payload.end());
    control = static_cast<unsigned char>((ashSendSequence_ << 4) | ashReceiveSequence_);
    ashSendSequence_ = static_cast<unsigned char>((ashSendSequence_ + 1) & 7);
    encoded = AshTransport::encodeData(control, randomize(ezsp));
    writeBytes(encoded);
}

void Gateway::handleAshFrame(const std::vector<unsigned char>& frame) {
    unsigned char control;
    if (frame.empty()) return;
    control = frame[0];
    if (control == 0xc1) {
        ashSendSequence_ = 0;
        ashReceiveSequence_ = 0;
        resetDeadline_ = 0;
        std::vector<unsigned char> version;
        version.push_back(ezspVersion_);
        sendEzspCommand(0x0000, version);
    } else if ((control & 0x80) == 0) {
        std::vector<unsigned char> data(frame.begin() + 1, frame.end());
        std::vector<unsigned char> ack = AshTransport::encodeData(
            static_cast<unsigned char>(0x80 | ((control >> 4) + 1) % 8), std::vector<unsigned char>());
        ashReceiveSequence_ = static_cast<unsigned char>(((control >> 4) + 1) & 7);
        writeBytes(ack);
        handleEzspFrame(randomize(data));
    }
}

void Gateway::handleEzspFrame(const std::vector<unsigned char>& frame) {
    unsigned short command;
    if (frame.size() < 3) return;
    if (ezspVersion_ >= 8) {
        if (frame.size() < 5) return;
        command = static_cast<unsigned short>(frame[3]) | (static_cast<unsigned short>(frame[4]) << 8);
        if (command == 0 && frame.size() > 5) ezspVersion_ = frame[5];
    } else {
        command = frame[2];
        if (command == 0 && frame.size() > 3) ezspVersion_ = frame[3];
    }
    if (command == 0) online_ = true;
}
