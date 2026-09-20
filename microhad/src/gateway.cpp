#include "gateway.h"
#include "storage.h"
#include "zcl.h"
#include "fcm.h"
#include "web_push.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <termios.h>
#include <unistd.h>

Device::Device() : networkAddress(0xffff), kind("generic"), contactOpen(false), hasContact(false), hasTemperature(false),
                   temperatureCelsius(0), hasBattery(false), batteryPercent(0), linkQuality(0), lastSeen(0),
                   iasZoneId(-1), hasLowTemperatureAlert(false), hasHighTemperatureAlert(false), lowTemperatureAlert(0),
                   highTemperatureAlert(0) {
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

unsigned long zclValueLength(unsigned char type, const std::vector<unsigned char>& message, unsigned long offset) {
    if (type == 0x10 || type == 0x20 || type == 0x28 || type == 0x30) return 1;
    if (type == 0x21 || type == 0x29 || type == 0x31) return 2;
    if (type == 0x23 || type == 0x2b || type == 0x39) return 4;
    if (type == 0x42 && offset < message.size()) return 1 + message[offset];
    if (type == 0x43 && offset + 1 < message.size()) {
        return 2 + static_cast<unsigned long>(message[offset]) + (static_cast<unsigned long>(message[offset + 1]) << 8);
    }
    return 0;
}
}

Gateway::Gateway(const std::string& serialDevice, int serialBaud, const std::string& runtimeDirectory,
                 const std::string& firebaseServiceAccount, const std::string& firebaseTopic, const std::string& caBundle) :
    serialDevice_(serialDevice), serialBaud_(serialBaud), runtimeDirectory_(runtimeDirectory),
    firebaseServiceAccount_(firebaseServiceAccount), firebaseTopic_(firebaseTopic), caBundle_(caBundle), serialFd_(-1),
    online_(false), joinUntil_(0), joinPermitRenewAt_(0), pendingJoinSeconds_(0), joinPreparationStep_(0), ashSendSequence_(0), ashReceiveSequence_(0), ezspSequence_(0),
    ezspVersion_(4), ezspVersionNegotiated_(false), resetDeadline_(0), networkForming_(false), networkFormed_(false), networkPanId_(0),
    networkChannel_(0) {
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
        sendJoinPermit(0);
        joinUntil_ = 0;
        joinPermitRenewAt_ = 0;
    } else if (joinUntil_ != 0 && time(0) >= joinPermitRenewAt_) {
        long remaining = joinUntil_ - time(0);
        unsigned int duration = remaining > 254 ? 254 : static_cast<unsigned int>(remaining);
        if (duration != 0) {
            sendJoinPermit(duration);
            joinPermitRenewAt_ = time(0) + (duration > 240 ? 240 : duration);
        }
    }
    std::map<std::string, long>::iterator alert;
    long now = time(0);
    for (alert = nextContactAlertAt_.begin(); alert != nextContactAlertAt_.end(); ++alert) {
        std::map<std::string, Device>::const_iterator device = devices_.find(alert->first);
        if (device != devices_.end() && device->second.contactOpen && now >= alert->second) {
            emitNotification("Window open", device->second.name.empty() ? device->second.ieeeAddress : device->second.name);
            alert->second = now + 300;
        }
    }
}

bool Gateway::isOnline() const {
    return online_;
}

std::string Gateway::runtimeDirectory() const {
    return runtimeDirectory_;
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
    if (seconds == 0) {
        sendJoinPermit(0);
        joinUntil_ = 0;
        joinPermitRenewAt_ = 0;
        pendingJoinSeconds_ = 0;
        joinPreparationStep_ = 0;
    } else if (ezspVersion_ >= 8) {
        std::vector<unsigned char> payload;
        static const char key[] = "ZigBeeAlliance09";
        unsigned long i;
        for (i = 0; i < 8; ++i) payload.push_back(0xff); /* wildcard EUI64 */
        for (i = 0; i < sizeof(key) - 1; ++i) payload.push_back(static_cast<unsigned char>(key[i]));
        pendingJoinSeconds_ = seconds;
        joinPreparationStep_ = 1;
        sendEzspCommand(0x00af, payload); /* addTransientLinkKey */
    } else {
        beginJoinWindow(seconds);
    }
    return true;
}

bool Gateway::createFreshNetwork(std::string* error) {
    if (!online_) { *error = "Zigbee coordinator is offline"; return false; }
    if (networkForming_) { *error = "network creation is already in progress"; return false; }
    if (!generateNetworkMaterial(error)) return false;
    networkForming_ = true;
    networkFormed_ = false;
    sendEzspCommand(0x00b1, std::vector<unsigned char>()); /* clearKeyTable */
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
    bool wasOpen = device.contactOpen;
    bool hadTemperature = device.hasTemperature;
    double previousTemperature = device.temperatureCelsius;
    if (device.ieeeAddress.empty()) device.ieeeAddress = ieeeAddress;
    changed = applyZclAttribute(&device, cluster, attribute, value, time(0));
    if (changed) { updateContactAlert(ieeeAddress, wasOpen); updateTemperatureAlert(device, hadTemperature, previousTemperature); saveState(); }
    return changed;
}

bool Gateway::setTemperatureAlerts(const std::string& ieeeAddress, bool hasLow, double low, bool hasHigh, double high,
                                   std::string* error) {
    std::map<std::string, Device>::iterator device = devices_.find(ieeeAddress);
    if (device == devices_.end()) { *error = "device not found"; return false; }
    if ((hasLow && (low < -100 || low > 200)) || (hasHigh && (high < -100 || high > 200)) ||
        (hasLow && hasHigh && low >= high)) { *error = "invalid temperature thresholds"; return false; }
    device->second.hasLowTemperatureAlert = hasLow;
    device->second.lowTemperatureAlert = low;
    device->second.hasHighTemperatureAlert = hasHigh;
    device->second.highTemperatureAlert = high;
    saveState();
    return true;
}

bool Gateway::applyIasZoneStatus(const std::string& ieeeAddress, const std::vector<unsigned char>& payload) {
    Device& device = devices_[ieeeAddress];
    bool changed;
    bool wasOpen = device.contactOpen;
    if (device.ieeeAddress.empty()) device.ieeeAddress = ieeeAddress;
    changed = ::applyIasZoneStatus(&device, payload, time(0));
    if (changed) { updateContactAlert(ieeeAddress, wasOpen); saveState(); }
    return changed;
}

void Gateway::updateContactAlert(const std::string& ieeeAddress, bool wasOpen) {
    std::map<std::string, Device>::const_iterator device = devices_.find(ieeeAddress);
    if (device == devices_.end() || !device->second.hasContact) return;
    if (device->second.contactOpen && !wasOpen) {
        contactOpenedAt_[ieeeAddress] = time(0);
        emitNotification("Contact opened", device->second.name.empty() ? device->second.ieeeAddress : device->second.name);
        nextContactAlertAt_[ieeeAddress] = time(0) + 600;
    } else if (!device->second.contactOpen && wasOpen) {
        contactOpenedAt_.erase(ieeeAddress);
        nextContactAlertAt_.erase(ieeeAddress);
    }
}

void Gateway::sendTestNotification() {
    emitNotification("MicroHAD test", "Push notifications are configured.");
}

void Gateway::updateTemperatureAlert(const Device& device, bool hadTemperature, double previousTemperature) {
    if (!device.hasTemperature || !hadTemperature) return;
    if (device.hasHighTemperatureAlert && previousTemperature < device.highTemperatureAlert &&
        device.temperatureCelsius >= device.highTemperatureAlert) {
        emitNotification("Temperature high", device.name.empty() ? device.ieeeAddress : device.name);
    }
    if (device.hasLowTemperatureAlert && previousTemperature > device.lowTemperatureAlert &&
        device.temperatureCelsius <= device.lowTemperatureAlert) {
        emitNotification("Temperature low", device.name.empty() ? device.ieeeAddress : device.name);
    }
}

void Gateway::emitNotification(const std::string& title, const std::string& body) {
    std::ofstream output((runtimeDirectory_ + "/notification-outbox.ndjson").c_str(), std::ios::app);
    if (output) output << "{\"title\":\"" << jsonEscape(title) << "\",\"body\":\""
                       << jsonEscape(body) << "\",\"timestamp\":" << time(0) << "}\n";
    if (!firebaseServiceAccount_.empty() && !firebaseTopic_.empty() && !caBundle_.empty()) {
        std::string ignored;
        sendFcmNotification(firebaseServiceAccount_, caBundle_, firebaseTopic_, title, body, &ignored);
    }
    if (!caBundle_.empty()) {
        std::string ignored;
        WebPushStore browserPush(runtimeDirectory_);
        browserPush.send(caBundle_, title, body, &ignored);
    }
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
    json << "{\"online\":" << (online_ ? "true" : "false") << ",\"network_forming\":"
         << (networkForming_ ? "true" : "false") << ",\"network_formed\":" << (networkFormed_ ? "true" : "false")
         << ",\"joining_until\":" << joinUntil_
         << ",\"devices\":[";
    std::map<std::string, Device>::const_iterator it;
    bool first = true;
    for (it = devices_.begin(); it != devices_.end(); ++it) {
        const Device& device = it->second;
        if (!first) json << ',';
        first = false;
        json << "{\"ieee_address\":\"" << jsonEscape(device.ieeeAddress) << "\",\"name\":\""
             << jsonEscape(device.name) << "\",\"kind\":\"" << jsonEscape(device.kind)
             << "\",\"has_contact\":" << (device.hasContact ? "true" : "false")
             << ",\"contact_open\":" << (device.contactOpen ? "true" : "false")
             << ",\"has_temperature\":" << (device.hasTemperature ? "true" : "false")
             << ",\"temperature_celsius\":" << device.temperatureCelsius
             << ",\"has_battery\":" << (device.hasBattery ? "true" : "false")
             << ",\"battery_percent\":" << device.batteryPercent << ",\"link_quality\":" << device.linkQuality
             << ",\"has_low_temperature_alert\":" << (device.hasLowTemperatureAlert ? "true" : "false")
             << ",\"low_temperature_alert\":" << device.lowTemperatureAlert
             << ",\"has_high_temperature_alert\":" << (device.hasHighTemperatureAlert ? "true" : "false")
             << ",\"high_temperature_alert\":" << device.highTemperatureAlert
             << ",\"last_seen\":" << device.lastSeen << "}";
    }
    json << "]}";
    return json.str();
}

void Gateway::loadState() {
    std::vector<Device> saved;
    NetworkState network;
    std::string error;
    unsigned long i;
    if (loadDevices(runtimeDirectory_ + "/devices.json", &saved, &error)) {
        for (i = 0; i < saved.size(); ++i) {
            devices_[saved[i].ieeeAddress] = saved[i];
            if (saved[i].networkAddress != 0xffff) nodeAddresses_[saved[i].networkAddress] = saved[i].ieeeAddress;
        }
    }
    error.clear();
    if (loadNetworkState(runtimeDirectory_ + "/network.json", &network, &error) && network.formed) {
        networkFormed_ = true;
        networkPanId_ = network.panId;
        networkChannel_ = network.channel;
        networkExtendedPan_ = network.extendedPan;
        networkKey_ = network.networkKey;
        preconfiguredKey_ = network.preconfiguredKey;
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

void Gateway::sendApsUnicast(unsigned short destination, unsigned short profile, unsigned short cluster,
                             unsigned char sourceEndpoint, unsigned char destinationEndpoint,
                             const std::vector<unsigned char>& message) {
    std::vector<unsigned char> payload;
    static unsigned char apsSequence = 0;
    payload.push_back(0); /* EMBER_OUTGOING_DIRECT */
    payload.push_back(static_cast<unsigned char>(destination));
    payload.push_back(static_cast<unsigned char>(destination >> 8));
    payload.push_back(static_cast<unsigned char>(profile));
    payload.push_back(static_cast<unsigned char>(profile >> 8));
    payload.push_back(static_cast<unsigned char>(cluster));
    payload.push_back(static_cast<unsigned char>(cluster >> 8));
    payload.push_back(sourceEndpoint);
    payload.push_back(destinationEndpoint);
    payload.push_back(0); payload.push_back(0); /* APS options */
    payload.push_back(0); payload.push_back(0); /* group id */
    payload.push_back(apsSequence++);
    payload.push_back(0); /* message tag */
    payload.push_back(static_cast<unsigned char>(message.size()));
    payload.insert(payload.end(), message.begin(), message.end());
    sendEzspCommand(0x0034, payload); /* sendUnicast */
}

void Gateway::sendJoinPermit(unsigned int duration) {
    std::vector<unsigned char> payload;
    payload.push_back(static_cast<unsigned char>(duration > 254 ? 254 : duration));
    sendEzspCommand(0x0022, payload);
}

void Gateway::beginJoinWindow(unsigned int seconds) {
    unsigned int duration = seconds > 254 ? 254 : seconds;
    sendJoinPermit(duration);
    joinUntil_ = time(0) + seconds;
    joinPermitRenewAt_ = time(0) + (duration > 240 ? 240 : duration);
    pendingJoinSeconds_ = 0;
    joinPreparationStep_ = 0;
}

void Gateway::handleAshFrame(const std::vector<unsigned char>& frame) {
    unsigned char control;
    if (frame.empty()) return;
    control = frame[0];
    if (control == 0xc1) {
        ashSendSequence_ = 0;
        ashReceiveSequence_ = 0;
        resetDeadline_ = 0;
        ezspVersion_ = 4;
        ezspVersionNegotiated_ = false;
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
    unsigned long payloadOffset;
    if (frame.size() < 3) return;
    if (ezspVersion_ >= 8) {
        if (frame.size() < 5) return;
        command = static_cast<unsigned short>(frame[3]) | (static_cast<unsigned short>(frame[4]) << 8);
        payloadOffset = 5;
        if (command == 0 && frame.size() > payloadOffset) {
            unsigned char reportedVersion = frame[payloadOffset];
            if (reportedVersion != ezspVersion_) {
                std::vector<unsigned char> version;
                ezspVersion_ = reportedVersion;
                version.push_back(ezspVersion_);
                sendEzspCommand(0x0000, version);
                return;
            }
            ezspVersionNegotiated_ = true;
        }
    } else {
        command = frame[2];
        payloadOffset = 3;
        /* EZSP v5-v7 uses the legacy extended-command sentinel (0xff), then
           a reserved byte and the real 8-bit command identifier. */
        if (command == 0xff && frame.size() >= 5) {
            command = frame[4];
            payloadOffset = 5;
        }
        if (command == 0 && frame.size() > payloadOffset) {
            unsigned char reportedVersion = frame[payloadOffset];
            if (reportedVersion != ezspVersion_) {
                std::vector<unsigned char> version;
                ezspVersion_ = reportedVersion;
                version.push_back(ezspVersion_);
                sendEzspCommand(0x0000, version);
                return;
            }
            ezspVersionNegotiated_ = true;
        }
    }
    if (command == 0 && ezspVersionNegotiated_) online_ = true;
    if (networkForming_) {
        unsigned int status = frame.size() > payloadOffset ? frame[payloadOffset] : 0xff;
        fprintf(stderr, "EZSP formation response: version=%u command=0x%04x status=0x%02x bytes=%lu\n",
                static_cast<unsigned int>(ezspVersion_), static_cast<unsigned int>(command), status,
                static_cast<unsigned long>(frame.size()));
    }
    if (networkForming_ && (command == 0x00b1 || command == 0x0068 || command == 0x001e)) {
        if (frame[payloadOffset] == 0) continueNetworkFormation(command);
        else networkForming_ = false;
    }
    if (frame.size() <= payloadOffset) return;
    if (joinPreparationStep_ == 1 && command == 0x00af) {
        if (frame[payloadOffset] == 0) {
            std::vector<unsigned char> policy;
            policy.push_back(0); /* TRUST_CENTER_POLICY */
            policy.push_back(3); policy.push_back(0); /* allow joins and unsecured rejoins */
            joinPreparationStep_ = 2;
            sendEzspCommand(0x0055, policy);
        } else {
            fprintf(stderr, "cannot add Zigbee transient joining key: 0x%02x\n", frame[payloadOffset]);
            joinPreparationStep_ = 0;
            pendingJoinSeconds_ = 0;
        }
        return;
    }
    if (joinPreparationStep_ == 2 && command == 0x0055) {
        if (frame[payloadOffset] == 0) beginJoinWindow(pendingJoinSeconds_);
        else {
            fprintf(stderr, "cannot set Zigbee join policy: 0x%02x\n", frame[payloadOffset]);
            joinPreparationStep_ = 0;
            pendingJoinSeconds_ = 0;
        }
        return;
    }
    if (command == 0x0022 && frame[payloadOffset] != 0) {
        fprintf(stderr, "EZSP permit joining failed: 0x%02x\n", frame[payloadOffset]);
        joinUntil_ = 0;
        joinPermitRenewAt_ = 0;
    }
    if (command == 0x24 && frame.size() >= payloadOffset + 10) {
        unsigned short nodeId = static_cast<unsigned short>(frame[payloadOffset]) |
                               (static_cast<unsigned short>(frame[payloadOffset + 1]) << 8);
        registerNode(nodeId, &frame[payloadOffset + 2]);
    } else if (command == 0x23 && frame.size() >= payloadOffset + 12) {
        unsigned short nodeId = static_cast<unsigned short>(frame[payloadOffset + 2]) |
                               (static_cast<unsigned short>(frame[payloadOffset + 3]) << 8);
        registerNode(nodeId, &frame[payloadOffset + 4]);
    } else if (command == 0x45) {
        std::vector<unsigned char> payload(frame.begin() + payloadOffset, frame.end());
        handleIncomingMessage(payload);
    }
}

bool Gateway::generateNetworkMaterial(std::string* error) {
    int randomFd = open("/dev/urandom", O_RDONLY);
    unsigned char material[42];
    ssize_t count;
    if (randomFd < 0) { *error = "cannot read system random source"; return false; }
    count = read(randomFd, material, sizeof(material));
    close(randomFd);
    if (count != static_cast<ssize_t>(sizeof(material))) { *error = "cannot generate network material"; return false; }
    networkExtendedPan_.assign(material, material + 8);
    networkKey_.assign(material + 8, material + 24);
    preconfiguredKey_.assign(material + 24, material + 40);
    networkPanId_ = static_cast<unsigned short>(material[40]) | (static_cast<unsigned short>(material[41]) << 8);
    if (networkPanId_ == 0 || networkPanId_ == 0xffff) networkPanId_ = 0x1a62;
    networkChannel_ = static_cast<unsigned char>(11 + material[0] % 16);
    return true;
}

void Gateway::continueNetworkFormation(unsigned short completedCommand) {
    std::vector<unsigned char> payload;
    if (completedCommand == 0x00b1) {
        /* EmberInitialSecurityState, all multi-byte values little-endian. */
        unsigned short bitmask = 0x0100 | 0x0004 | 0x0200 | 0x0800 | 0x0084;
        payload.push_back(static_cast<unsigned char>(bitmask));
        payload.push_back(static_cast<unsigned char>(bitmask >> 8));
        unsigned long i;
        payload.insert(payload.end(), preconfiguredKey_.begin(), preconfiguredKey_.end());
        payload.insert(payload.end(), networkKey_.begin(), networkKey_.end());
        payload.push_back(0); /* network key sequence */
        for (i = 0; i < 8; ++i) payload.push_back(0);
        sendEzspCommand(0x0068, payload);
    } else if (completedCommand == 0x0068) {
        payload.insert(payload.end(), networkExtendedPan_.begin(), networkExtendedPan_.end());
        payload.push_back(static_cast<unsigned char>(networkPanId_));
        payload.push_back(static_cast<unsigned char>(networkPanId_ >> 8));
        payload.push_back(5); payload.push_back(networkChannel_); payload.push_back(0);
        payload.push_back(0); payload.push_back(0); payload.push_back(0);
        payload.push_back(0x00); payload.push_back(0xf8); payload.push_back(0xff); payload.push_back(0x07);
        sendEzspCommand(0x001e, payload);
    } else if (completedCommand == 0x001e) {
        networkForming_ = false;
        networkFormed_ = true;
        NetworkState network;
        std::string ignored;
        network.formed = true;
        network.panId = networkPanId_;
        network.channel = networkChannel_;
        network.extendedPan = networkExtendedPan_;
        network.networkKey = networkKey_;
        network.preconfiguredKey = preconfiguredKey_;
        saveNetworkState(runtimeDirectory_ + "/network.json", network, &ignored);
    }
}

void Gateway::registerNode(unsigned short nodeId, const unsigned char* eui64) {
    std::ostringstream address;
    int i;
    address << "0x" << std::hex << std::setfill('0');
    /* EZSP transmits EUI64 least significant byte first. */
    for (i = 7; i >= 0; --i) address << std::setw(2) << static_cast<unsigned int>(eui64[i]);
    nodeAddresses_[nodeId] = address.str();
    Device& device = devices_[address.str()];
    device.networkAddress = nodeId;
    if (device.ieeeAddress.empty()) {
        device.ieeeAddress = address.str();
        device.name = address.str();
        device.lastSeen = time(0);
    }
    saveState();
}

int Gateway::allocateIasZoneId(const std::string& ieeeAddress) {
    std::map<std::string, Device>::iterator current = devices_.find(ieeeAddress);
    int candidate;
    if (current != devices_.end() && current->second.iasZoneId >= 0 && current->second.iasZoneId <= 254) {
        return current->second.iasZoneId;
    }
    for (candidate = 0; candidate <= 254; ++candidate) {
        std::map<std::string, Device>::const_iterator device;
        bool used = false;
        for (device = devices_.begin(); device != devices_.end(); ++device) {
            if (device->second.iasZoneId == candidate) { used = true; break; }
        }
        if (!used) {
            devices_[ieeeAddress].iasZoneId = candidate;
            saveState();
            return candidate;
        }
    }
    return -1;
}

void Gateway::sendIasEnrollResponse(unsigned short destination, unsigned char sourceEndpoint,
                                    unsigned char destinationEndpoint, unsigned char transactionSequence,
                                    unsigned char zoneId) {
    std::vector<unsigned char> response;
    response.push_back(0x19); /* cluster-specific, server-to-client, default response disabled */
    response.push_back(transactionSequence);
    response.push_back(0x00); /* Zone Enroll Response */
    response.push_back(0x00); /* success */
    response.push_back(zoneId);
    sendApsUnicast(destination, 0x0104, 0x0500, sourceEndpoint, destinationEndpoint, response);
}

void Gateway::handleIncomingMessage(const std::vector<unsigned char>& payload) {
    unsigned short cluster;
    unsigned short sender;
    unsigned long messageOffset;
    unsigned long length;
    std::map<unsigned short, std::string>::const_iterator address;
    std::vector<unsigned char> message;
    if (payload.size() < 20) return;
    /* type(1), APS frame(11), LQI/RSSI(2), sender(2), binding/address(2), LVBytes */
    cluster = static_cast<unsigned short>(payload[3]) | (static_cast<unsigned short>(payload[4]) << 8);
    sender = static_cast<unsigned short>(payload[14]) | (static_cast<unsigned short>(payload[15]) << 8);
    messageOffset = 18;
    length = payload[messageOffset++];
    if (messageOffset + length > payload.size() || length < 3) return;
    message.assign(payload.begin() + messageOffset, payload.begin() + messageOffset + length);
    if (cluster == 0x0013 && message.size() >= 12) {
        /* ZDO Device_annce: transaction sequence, network address, EUI64, capability. */
        unsigned short announcedNode = static_cast<unsigned short>(message[1]) |
                                      (static_cast<unsigned short>(message[2]) << 8);
        registerNode(announcedNode, &message[3]);
        return;
    }
    address = nodeAddresses_.find(sender);
    if (address == nodeAddresses_.end()) return;
    unsigned char frameControl = message[0];
    unsigned long zclOffset = 3;
    if ((frameControl & 0x04) != 0) zclOffset += 2;
    if (zclOffset > message.size()) return;
    unsigned char command = message[zclOffset - 1];
    {
        std::map<unsigned short, std::string>::iterator known = nodeAddresses_.find(sender);
        if (known != nodeAddresses_.end()) {
            Device& device = devices_[known->second];
            device.linkQuality = payload[12];
            device.lastSeen = time(0);
        }
    }
    if (cluster == 0x0500 && command == 0x01 && message.size() >= zclOffset + 4) {
        int zoneId = allocateIasZoneId(address->second);
        if (zoneId >= 0) {
            sendIasEnrollResponse(sender, payload[5], payload[4], message[zclOffset - 2],
                                  static_cast<unsigned char>(zoneId));
        }
    } else if (cluster == 0x0500 && command == 0x00 && message.size() >= zclOffset + 2) {
        std::vector<unsigned char> zone(message.begin() + zclOffset, message.end());
        applyIasZoneStatus(address->second, zone);
    } else if (command == 0x0a) {
        unsigned long attributeOffset = zclOffset;
        while (attributeOffset + 3 <= message.size()) {
            unsigned short attribute = static_cast<unsigned short>(message[attributeOffset]) |
                                       (static_cast<unsigned short>(message[attributeOffset + 1]) << 8);
            unsigned char type = message[attributeOffset + 2];
            unsigned long valueOffset = attributeOffset + 3;
            unsigned long valueLength = zclValueLength(type, message, valueOffset);
            if (valueLength == 0 || valueOffset + valueLength > message.size()) break;
            std::vector<unsigned char> value(message.begin() + valueOffset, message.begin() + valueOffset + valueLength);
            applyAttributeReport(address->second, cluster, attribute, value);
            attributeOffset = valueOffset + valueLength;
        }
    }
}
