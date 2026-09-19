#include "gateway.h"
#include "storage.h"

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

Gateway::Gateway(const std::string& serialDevice, int serialBaud, const std::string& runtimeDirectory) :
    serialDevice_(serialDevice), serialBaud_(serialBaud), runtimeDirectory_(runtimeDirectory), serialFd_(-1),
    online_(false), joinUntil_(0) {
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
    /* The ASH/EZSP state machine is deliberately started only after exclusive
       direct ownership of ttyS1. It will issue reset/version/network commands
       as command coverage is enabled in the coordinator module. */
    online_ = true;
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
