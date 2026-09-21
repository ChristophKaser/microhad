#include "gateway.h"
#include "notification_worker.h"
#include "storage.h"
#include "zcl.h"
#include "fcm.h"
#include "web_push.h"

#include <cerrno>
#include <climits>
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
                   temperatureCelsius(0), hasHumidity(false), humidityPercent(0), hasOccupancy(false), occupied(false),
                   hasBattery(false), batteryPercent(0), linkQuality(0), lastSeen(0),
                   iasZoneId(-1), hasLowTemperatureAlert(false), hasHighTemperatureAlert(false), lowTemperatureAlert(0),
                   highTemperatureAlert(0), contactInitialAlertMinutes(10), contactReminderAlertMinutes(5) {
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
    if (type == 0x08 || type == 0x10 || type == 0x18 || type == 0x20 || type == 0x28 || type == 0x30) return 1;
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
                 const std::string& firebaseServiceAccount, const std::string& firebaseTopic, const std::string& caBundle,
                 NotificationWorker* notificationWorker) :
    serialDevice_(serialDevice), serialBaud_(serialBaud), runtimeDirectory_(runtimeDirectory),
    firebaseServiceAccount_(firebaseServiceAccount), firebaseTopic_(firebaseTopic), caBundle_(caBundle), notificationWorker_(notificationWorker), serialFd_(-1),
    online_(false), joinUntil_(0), joinPermitRenewAt_(0), pendingJoinSeconds_(0), joinPreparationStep_(0), ashSendSequence_(0), ashReceiveSequence_(0), ezspSequence_(0),
    ezspVersion_(4), zdoSequence_(0), zclTransactionSequence_(0), ezspVersionNegotiated_(false), stackTokenWritingRequested_(false), networkInitRequested_(false), coordinatorInitializationStep_(0), resetDeadline_(0), nextHeartbeatAt_(0), serialRxBytes_(0), ashRxFrames_(0), networkForming_(false), networkFormationStep_(0), networkFormed_(false), networkPanId_(0),
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
    loadLogs();
    loadState();
    if (!configureSerial(error)) {
        addLog(std::string("Serial startup failed: ") + *error);
        return false;
    }
    addLog(std::string("Gateway starting on ") + serialDevice_);
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
            serialRxBytes_ += static_cast<unsigned long>(count);
            addHexLog("SERIAL RX", bytes, static_cast<unsigned long>(count));
            ash_.feed(bytes, static_cast<unsigned long>(count));
            while (ash_.popFrame(&frame)) handleAshFrame(frame);
        }
    }
    if (online_ && nextHeartbeatAt_ != 0 && time(0) >= nextHeartbeatAt_) {
        addLog("Coordinator heartbeat");
        sendEzspCommand(0x0005, std::vector<unsigned char>()); /* nop */
        nextHeartbeatAt_ = time(0) + 20;
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
            emitNotification("Window open", device->second.name.empty() ? device->second.ieeeAddress : device->second.name, 0);
            alert->second = now + static_cast<long>(device->second.contactReminderAlertMinutes * 60);
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
        addLog("Pairing request rejected: coordinator offline");
        return false;
    }
    if (seconds > 3600) {
        *error = "joining window must not exceed one hour";
        addLog("Pairing request rejected: duration exceeds one hour");
        return false;
    }
    if (seconds == 0) {
        sendJoinPermit(0);
        joinUntil_ = 0;
        joinPermitRenewAt_ = 0;
        pendingJoinSeconds_ = 0;
        joinPreparationStep_ = 0;
        addLog("Pairing closed");
    } else if (ezspVersion_ >= 8) {
        pendingJoinSeconds_ = seconds;
        joinPreparationStep_ = 1;
        std::vector<unsigned char> policy;
        policy.push_back(6); /* APP_KEY_REQUEST_POLICY */
        policy.push_back(0x60); policy.push_back(0x00); /* allow app-key requests */
        addLog("Preparing Zigbee key-request policies");
        sendEzspCommand(0x0055, policy);
    } else {
        beginJoinWindow(seconds);
    }
    return true;
}

bool Gateway::createFreshNetwork(std::string* error) {
    bool leaveExisting;
    if (!online_) { *error = "Zigbee coordinator is offline"; return false; }
    if (networkForming_) { *error = "network creation is already in progress"; return false; }
    if (!generateNetworkMaterial(error)) return false;
    networkForming_ = true;
    leaveExisting = networkFormed_;
    networkFormed_ = false;
    if (leaveExisting) {
        networkFormationStep_ = 1;
        addLog("Leaving the existing Zigbee network before replacement");
        sendEzspCommand(0x0020, std::vector<unsigned char>()); /* leaveNetwork */
    } else {
        networkFormationStep_ = 2;
        addLog("Creating replacement Zigbee network from network-down state");
        sendEzspCommand(0x00b1, std::vector<unsigned char>()); /* clearKeyTable */
    }
    return true;
}

bool Gateway::setDeviceSettings(const std::string& ieeeAddress, const std::string& name,
                                bool hasLow, double low, bool hasHigh, double high,
                                bool hasContactTimings, unsigned long initialAlertMinutes,
                                unsigned long reminderAlertMinutes, std::string* error) {
    std::map<std::string, Device>::iterator it = devices_.find(ieeeAddress);
    if (it == devices_.end()) {
        *error = "device not found";
        return false;
    }
    if (name.empty() || name.size() > 64) {
        *error = "device name must be between 1 and 64 characters";
        return false;
    }
    if (it->second.hasTemperature &&
        ((hasLow && (low < -100 || low > 200)) || (hasHigh && (high < -100 || high > 200)) ||
         (hasLow && hasHigh && low >= high))) {
        *error = "invalid temperature thresholds";
        return false;
    }
    if (it->second.hasContact) {
        long now = time(0);
        if (!hasContactTimings || initialAlertMinutes == 0 || reminderAlertMinutes == 0 ||
            initialAlertMinutes > static_cast<unsigned long>((LONG_MAX - now) / 60) ||
            reminderAlertMinutes > static_cast<unsigned long>((LONG_MAX - now) / 60)) {
            *error = "invalid contact notification times";
            return false;
        }
    }
    it->second.name = name;
    if (it->second.hasTemperature) {
        it->second.hasLowTemperatureAlert = hasLow;
        it->second.lowTemperatureAlert = low;
        it->second.hasHighTemperatureAlert = hasHigh;
        it->second.highTemperatureAlert = high;
    }
    if (it->second.hasContact) {
        long now = time(0);
        it->second.contactInitialAlertMinutes = initialAlertMinutes;
        it->second.contactReminderAlertMinutes = reminderAlertMinutes;
        if (it->second.contactOpen) {
            long openedAt = contactOpenedAt_.count(ieeeAddress) ? contactOpenedAt_[ieeeAddress] : now;
            long dueAt = openedAt + static_cast<long>(initialAlertMinutes * 60);
            contactOpenedAt_[ieeeAddress] = openedAt;
            nextContactAlertAt_[ieeeAddress] = dueAt < now ? now : dueAt;
        }
    }
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
        nextContactAlertAt_[ieeeAddress] = time(0) + static_cast<long>(device->second.contactInitialAlertMinutes * 60);
    } else if (!device->second.contactOpen && wasOpen) {
        contactOpenedAt_.erase(ieeeAddress);
        nextContactAlertAt_.erase(ieeeAddress);
    }
}

bool Gateway::sendTestNotification(std::string* error) {
    return emitNotification("MicroHAD test", "Push notifications are configured.", error);
}

bool Gateway::notificationsEnabled() const { return notificationWorker_ != 0 && notificationWorker_->enabled(); }

bool Gateway::setNotificationsEnabled(bool enabled, std::string* error) {
    if (notificationWorker_ == 0) { *error = "notification worker is unavailable"; return false; }
    if (!notificationWorker_->setEnabled(enabled, error)) return false;
    addLog(std::string("Push notifications ") + (enabled ? "enabled" : "disabled; pending notifications discarded"));
    return true;
}

void Gateway::updateTemperatureAlert(const Device& device, bool hadTemperature, double previousTemperature) {
    if (!device.hasTemperature || !hadTemperature) return;
    if (device.hasHighTemperatureAlert && previousTemperature < device.highTemperatureAlert &&
        device.temperatureCelsius >= device.highTemperatureAlert) {
        emitNotification("Temperature high", device.name.empty() ? device.ieeeAddress : device.name, 0);
    }
    if (device.hasLowTemperatureAlert && previousTemperature > device.lowTemperatureAlert &&
        device.temperatureCelsius <= device.lowTemperatureAlert) {
        emitNotification("Temperature low", device.name.empty() ? device.ieeeAddress : device.name, 0);
    }
}

bool Gateway::emitNotification(const std::string& title, const std::string& body, std::string* error) {
    if (notificationWorker_ == 0) { if (error != 0) *error = "no notification delivery is configured"; return false; }
    if (!notificationWorker_->enabled()) {
        addLog(std::string("Notification suppressed: ") + title + " - " + body);
        if (error != 0) *error = "push notifications are disabled";
        return false;
    }
    std::ofstream output((runtimeDirectory_ + "/notification-outbox.ndjson").c_str(), std::ios::app);
    addLog(std::string("Notification queued: ") + title + " - " + body);
    if (output) output << "{\"title\":\"" << jsonEscape(title) << "\",\"body\":\""
                       << jsonEscape(body) << "\",\"timestamp\":" << time(0) << "}\n";
    return notificationWorker_->enqueue(title, body, error);
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
         << ",\"serial_rx_bytes\":" << serialRxBytes_ << ",\"ash_rx_frames\":" << ashRxFrames_
         << ",\"joining_until\":" << joinUntil_ << ",\"joining_preparing\":"
         << (joinPreparationStep_ != 0 ? "true" : "false")
         << ",\"notifications_enabled\":" << (notificationsEnabled() ? "true" : "false")
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
             << ",\"has_humidity\":" << (device.hasHumidity ? "true" : "false")
             << ",\"humidity_percent\":" << device.humidityPercent
             << ",\"has_occupancy\":" << (device.hasOccupancy ? "true" : "false")
             << ",\"occupied\":" << (device.occupied ? "true" : "false")
             << ",\"has_battery\":" << (device.hasBattery ? "true" : "false")
             << ",\"battery_percent\":" << device.batteryPercent << ",\"link_quality\":" << device.linkQuality
             << ",\"has_low_temperature_alert\":" << (device.hasLowTemperatureAlert ? "true" : "false")
             << ",\"low_temperature_alert\":" << device.lowTemperatureAlert
             << ",\"has_high_temperature_alert\":" << (device.hasHighTemperatureAlert ? "true" : "false")
             << ",\"high_temperature_alert\":" << device.highTemperatureAlert
             << ",\"contact_initial_alert_minutes\":" << device.contactInitialAlertMinutes
             << ",\"contact_reminder_alert_minutes\":" << device.contactReminderAlertMinutes
             << ",\"last_seen\":" << device.lastSeen << "}";
    }
    json << "]}";
    return json.str();
}

std::string Gateway::logsJson() const {
    std::ostringstream json;
    unsigned long i;
    json << "{\"lines\":[";
    for (i = 0; i < logs_.size(); ++i) {
        if (i != 0) json << ',';
        json << "\"" << jsonEscape(logs_[i]) << "\"";
    }
    json << "]}";
    return json.str();
}

void Gateway::loadLogs() {
    std::ifstream input("/tmp/microhad-zigbee-native.log");
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) logs_.push_back(line);
    }
    while (logs_.size() > 512) logs_.erase(logs_.begin());
}

void Gateway::saveLogs() const {
    std::ofstream output("/tmp/microhad-zigbee-native.log", std::ios::trunc);
    unsigned long i;
    if (!output) return;
    for (i = 0; i < logs_.size(); ++i) output << logs_[i] << "\n";
}

void Gateway::addLog(const std::string& message) {
    std::ostringstream line;
    std::string rendered = message.substr(0, 240);
    line << time(0) << " " << rendered;
    logs_.push_back(line.str());
    while (logs_.size() > 512) logs_.erase(logs_.begin());
    saveLogs();
}

void Gateway::addHexLog(const std::string& direction, const unsigned char* bytes, unsigned long length) {
    const unsigned long bytesPerLine = 80;
    unsigned long offset;
    if (length == 0) {
        addLog(direction + " [0/0]");
        return;
    }
    for (offset = 0; offset < length; offset += bytesPerLine) {
        unsigned long end = offset + bytesPerLine;
        unsigned long i;
        std::ostringstream message;
        if (end > length) end = length;
        message << direction << " [" << offset << "/" << length << "] " << std::hex << std::setfill('0');
        for (i = offset; i < end; ++i) message << std::setw(2) << static_cast<unsigned int>(bytes[i]);
        addLog(message.str());
    }
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
    if (!bytes.empty()) addHexLog("SERIAL TX", &bytes[0], bytes.size());
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
    nextHeartbeatAt_ = 0;
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
    {
        std::ostringstream message;
        message << "EZSP -> 0x" << std::hex << command << " payload=" << std::dec << payload.size();
        addLog(message.str());
    }
    writeBytes(encoded);
}

unsigned short Gateway::coordinatorInitializationCommand() const {
    if (coordinatorInitializationStep_ >= 1 && coordinatorInitializationStep_ <= 11) return 0x0053;
    if (coordinatorInitializationStep_ >= 12 && coordinatorInitializationStep_ <= 14) return 0x0055;
    if (coordinatorInitializationStep_ >= 15 && coordinatorInitializationStep_ <= 16) return 0x00ab;
    if (coordinatorInitializationStep_ == 17) return 0x0010;
    if (coordinatorInitializationStep_ == 18) return 0x005a;
    if (coordinatorInitializationStep_ >= 19 && coordinatorInitializationStep_ <= 20) return 0x0002;
    if (coordinatorInitializationStep_ == 21) return 0x0017;
    return 0xffff;
}

void Gateway::continueCoordinatorInitialization() {
    static const unsigned char configurationIds[] = {56, 25, 29, 34, 42, 18, 19, 13, 12, 28, 1};
    static const unsigned short configurationValues[] = {90, 2, 50, 2, 3, 7680, 14, 5, 2, 1, 255};
    std::vector<unsigned char> payload;
    unsigned char step = coordinatorInitializationStep_;
    if (step < 11) {
        payload.push_back(configurationIds[step]);
        payload.push_back(static_cast<unsigned char>(configurationValues[step]));
        payload.push_back(static_cast<unsigned char>(configurationValues[step] >> 8));
        ++coordinatorInitializationStep_;
        sendEzspCommand(0x0053, payload); /* setConfigurationValue */
    } else if (step < 14) {
        static const unsigned char policyIds[] = {6, 5, 0};
        static const unsigned short decisions[] = {0x60, 0x51, 0x03};
        unsigned char policy = static_cast<unsigned char>(step - 11);
        payload.push_back(policyIds[policy]);
        payload.push_back(static_cast<unsigned char>(decisions[policy]));
        payload.push_back(static_cast<unsigned char>(decisions[policy] >> 8));
        ++coordinatorInitializationStep_;
        sendEzspCommand(0x0055, payload); /* setPolicy */
    } else if (step < 16) {
        static const unsigned char valueIds[] = {63, 21};
        static const unsigned char values[] = {3, 0};
        unsigned char value = static_cast<unsigned char>(step - 14);
        payload.push_back(valueIds[value]); payload.push_back(1); payload.push_back(values[value]);
        ++coordinatorInitializationStep_;
        sendEzspCommand(0x00ab, payload); /* setValue */
    } else if (step == 16) {
        const unsigned char concentrator[] = {1, 0xf9, 0xff, 10, 0, 90, 0, 4, 3, 0};
        payload.assign(concentrator, concentrator + sizeof(concentrator));
        ++coordinatorInitializationStep_;
        sendEzspCommand(0x0010, payload); /* setConcentrator */
    } else if (step == 17) {
        payload.push_back(1);
        ++coordinatorInitializationStep_;
        sendEzspCommand(0x005a, payload); /* setSourceRouteDiscoveryMode */
    } else if (step == 18) {
        static const unsigned short inputClusters[] = {0, 3, 6, 10, 25, 26, 768};
        static const unsigned short outputClusters[] = {0, 3, 4, 5, 6, 8, 32, 768, 1024, 1026, 1029,
                                                        1030, 1280, 2817, 2819, 2820, 1794, 4096, 64513, 64514};
        unsigned long i;
        payload.push_back(1); payload.push_back(0x04); payload.push_back(0x01);
        payload.push_back(0xef); payload.push_back(0xbe); payload.push_back(0);
        payload.push_back(7); payload.push_back(20);
        for (i = 0; i < 7; ++i) {
            payload.push_back(static_cast<unsigned char>(inputClusters[i]));
            payload.push_back(static_cast<unsigned char>(inputClusters[i] >> 8));
        }
        for (i = 0; i < 20; ++i) {
            payload.push_back(static_cast<unsigned char>(outputClusters[i]));
            payload.push_back(static_cast<unsigned char>(outputClusters[i] >> 8));
        }
        ++coordinatorInitializationStep_;
        sendEzspCommand(0x0002, payload); /* addEndpoint */
    } else if (step == 19) {
        const unsigned char greenPowerEndpoint[] = {242, 0xe0, 0xa1, 0x61, 0, 0, 0, 1, 0x21, 0};
        payload.assign(greenPowerEndpoint, greenPowerEndpoint + sizeof(greenPowerEndpoint));
        ++coordinatorInitializationStep_;
        sendEzspCommand(0x0002, payload); /* addEndpoint */
    } else if (step == 20) {
        ++coordinatorInitializationStep_;
        networkInitRequested_ = true;
        addLog("Initializing NCP Zigbee network after coordinator configuration");
        sendEzspCommand(0x0017, payload);
    }
}

void Gateway::enableStackTokenWriting() {
    std::vector<unsigned char> payload;
    payload.push_back(0x07); /* VALUE_STACK_TOKEN_WRITING */
    payload.push_back(1);    /* LVBytes length */
    payload.push_back(1);    /* enabled */
    stackTokenWritingRequested_ = true;
    addLog("Enabling NCP stack token writing");
    sendEzspCommand(0x00ab, payload);
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
    payload.push_back(0x40); payload.push_back(0x01); /* retry and route discovery */
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
    std::ostringstream message;
    message << "Pairing enabled for " << seconds << " seconds";
    addLog(message.str());
}

void Gateway::handleAshFrame(const std::vector<unsigned char>& frame) {
    unsigned char control;
    if (frame.empty()) return;
    ++ashRxFrames_;
    control = frame[0];
    if (control == 0xc1) {
        ashSendSequence_ = 0;
        ashReceiveSequence_ = 0;
        resetDeadline_ = 0;
        ezspVersion_ = 4;
        ezspVersionNegotiated_ = false;
        networkInitRequested_ = false;
        coordinatorInitializationStep_ = 0;
        addLog("ASH reset acknowledgement received");
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
    {
        std::ostringstream message;
        unsigned int status = frame.size() > payloadOffset ? frame[payloadOffset] : 0xff;
        message << "EZSP <- v" << std::dec << static_cast<unsigned int>(ezspVersion_) << " 0x" << std::hex
                << command << " status=0x" << status << " bytes=" << std::dec << frame.size();
        addLog(message.str());
    }
    if (command == 0 && ezspVersionNegotiated_) {
        online_ = true;
        nextHeartbeatAt_ = time(0) + 20;
        if (!networkInitRequested_) {
            addLog("Configuring NCP coordinator to match original controller");
            continueCoordinatorInitialization();
        }
    }
    if (coordinatorInitializationStep_ != 0 && command == coordinatorInitializationCommand()) {
        if (command != 0x005a && frame.size() > payloadOffset && frame[payloadOffset] != 0) {
            std::ostringstream failure;
            failure << "Coordinator initialization command 0x" << std::hex << command
                    << " rejected: 0x" << static_cast<unsigned int>(frame[payloadOffset]);
            addLog(failure.str());
        }
        if (coordinatorInitializationStep_ == 21) {
            coordinatorInitializationStep_ = 0;
        } else {
            continueCoordinatorInitialization();
            return;
        }
    }
    if (networkForming_) {
        unsigned int status = frame.size() > payloadOffset ? frame[payloadOffset] : 0xff;
        fprintf(stderr, "EZSP formation response: version=%u command=0x%04x status=0x%02x bytes=%lu\n",
                static_cast<unsigned int>(ezspVersion_), static_cast<unsigned int>(command), status,
                static_cast<unsigned long>(frame.size()));
    }
    if (networkForming_ && command == 0x0020) {
        if (frame[payloadOffset] != 0) {
            if (frame[payloadOffset] == 0x70) {
                networkFormationStep_ = 2;
                addLog("NCP already network-down; creating replacement network");
                sendEzspCommand(0x00b1, std::vector<unsigned char>()); /* clearKeyTable */
            } else {
                networkForming_ = false;
                networkFormationStep_ = 0;
                std::ostringstream failure;
                failure << "NCP rejected leave-network request: 0x" << std::hex
                        << static_cast<unsigned int>(frame[payloadOffset]);
                addLog(failure.str());
            }
        } else {
            addLog("NCP leaving old Zigbee network");
        }
        return;
    }
    if (networkForming_ && (command == 0x00b1 || command == 0x0068 || command == 0x001e)) {
        if (frame[payloadOffset] == 0) continueNetworkFormation(command);
        else {
            std::ostringstream failure;
            failure << "Network formation command 0x" << std::hex << command
                    << " rejected: 0x" << static_cast<unsigned int>(frame[payloadOffset]);
            addLog(failure.str());
            networkForming_ = false;
            networkFormationStep_ = 0;
        }
    }
    if (frame.size() <= payloadOffset) return;
    if (command == 0x0017) {
        if (frame[payloadOffset] == 0) addLog("NCP network initialization accepted");
        else {
            networkFormed_ = false;
            std::ostringstream message;
            message << "NCP network initialization rejected: 0x" << std::hex
                    << static_cast<unsigned int>(frame[payloadOffset]);
            addLog(message.str());
        }
        return;
    }
    if (command == 0x0019) {
        if (networkForming_ && networkFormationStep_ == 1 && frame[payloadOffset] != 0x90) {
            networkFormationStep_ = 2;
            addLog("Old Zigbee network left; creating replacement network");
            sendEzspCommand(0x00b1, std::vector<unsigned char>()); /* clearKeyTable */
            return;
        }
        if (frame[payloadOffset] == 0x90) {
            networkFormed_ = true;
            addLog("NCP Zigbee network is active");
        } else {
            networkFormed_ = false;
            std::ostringstream message;
            message << "NCP Zigbee network status: 0x" << std::hex
                    << static_cast<unsigned int>(frame[payloadOffset]);
            addLog(message.str());
        }
        return;
    }
    if (joinPreparationStep_ == 1 && command == 0x0055) {
        if (frame[payloadOffset] == 0) {
            std::vector<unsigned char> policy;
            policy.push_back(5); /* TC_KEY_REQUEST_POLICY */
            policy.push_back(0x51); policy.push_back(0x00); /* permit global trust-center key requests */
            joinPreparationStep_ = 2;
            sendEzspCommand(0x0055, policy);
        } else {
            fprintf(stderr, "cannot set Zigbee application-key policy: 0x%02x\n", frame[payloadOffset]);
            addLog("Cannot set Zigbee application-key policy");
            joinPreparationStep_ = 0;
            pendingJoinSeconds_ = 0;
        }
        return;
    }
    if (joinPreparationStep_ == 2 && command == 0x0055) {
        if (frame[payloadOffset] == 0) {
            std::vector<unsigned char> payload;
            static const char key[] = "ZigBeeAlliance09";
            unsigned long i;
            for (i = 0; i < 8; ++i) payload.push_back(0xff); /* wildcard EUI64 */
            for (i = 0; i < sizeof(key) - 1; ++i) payload.push_back(static_cast<unsigned char>(key[i]));
            joinPreparationStep_ = 3;
            addLog("Preparing secure pairing window");
            sendEzspCommand(0x00af, payload); /* addTransientLinkKey */
        }
        else {
            fprintf(stderr, "cannot set Zigbee trust-center key policy: 0x%02x\n", frame[payloadOffset]);
            addLog("Cannot set Zigbee trust-center key policy");
            joinPreparationStep_ = 0;
            pendingJoinSeconds_ = 0;
        }
        return;
    }
    if (joinPreparationStep_ == 3 && command == 0x00af) {
        if (frame[payloadOffset] == 0) {
            std::vector<unsigned char> policy;
            policy.push_back(0); /* TRUST_CENTER_POLICY */
            policy.push_back(3); policy.push_back(0); /* allow joins and unsecured rejoins */
            joinPreparationStep_ = 4;
            sendEzspCommand(0x0055, policy);
        } else {
            fprintf(stderr, "cannot add Zigbee transient joining key: 0x%02x\n", frame[payloadOffset]);
            addLog("Cannot add Zigbee transient joining key");
            joinPreparationStep_ = 0;
            pendingJoinSeconds_ = 0;
        }
        return;
    }
    if (joinPreparationStep_ == 4 && command == 0x0055) {
        if (frame[payloadOffset] == 0) beginJoinWindow(pendingJoinSeconds_);
        else {
            fprintf(stderr, "cannot set Zigbee join policy: 0x%02x\n", frame[payloadOffset]);
            addLog("Cannot set Zigbee join policy");
            joinPreparationStep_ = 0;
            pendingJoinSeconds_ = 0;
        }
        return;
    }
    if (command == 0x0022 && frame[payloadOffset] != 0) {
        fprintf(stderr, "EZSP permit joining failed: 0x%02x\n", frame[payloadOffset]);
        addLog("Coordinator rejected pairing permit");
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
    /* With TRUST_CENTER_USES_HASHED_LINK_KEY this field is a private random
       trust-center secret, not the public ZigBeeAlliance09 joining key.  The
       latter is installed separately as a transient wildcard key whenever a
       pairing window opens. */
    preconfiguredKey_.assign(material + 24, material + 40);
    networkPanId_ = static_cast<unsigned short>(material[40]) | (static_cast<unsigned short>(material[41]) << 8);
    if (networkPanId_ == 0 || networkPanId_ == 0xffff) networkPanId_ = 0x1a62;
    /* Keep the coordinator on the Zigbee default channel.  Besides making a
       freshly deployed gateway reproducible, channel 11 is the channel used
       by the vendor/original controller and is broadly supported by sleepy
       end devices during their short pairing scan. */
    networkChannel_ = 11;
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
        networkFormationStep_ = 0;
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
        addLog("Fresh Zigbee network formed");
        if (!stackTokenWritingRequested_) enableStackTokenWriting();
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
    std::ostringstream message;
    message << "Device announced: " << address.str() << " (0x" << std::hex << nodeId << ")";
    addLog(message.str());
    requestActiveEndpoints(nodeId);
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

void Gateway::requestActiveEndpoints(unsigned short destination) {
    std::vector<unsigned char> request;
    request.push_back(zdoSequence_++);
    request.push_back(static_cast<unsigned char>(destination));
    request.push_back(static_cast<unsigned char>(destination >> 8));
    sendApsUnicast(destination, 0x0000, 0x0005, 0, 0, request);
}

void Gateway::sendZclReadAttribute(unsigned short destination, unsigned char endpoint, unsigned short cluster,
                                   unsigned short attribute) {
    std::vector<unsigned char> request;
    request.push_back(0x10); /* global command, client-to-server, disable default response */
    request.push_back(zclTransactionSequence_++);
    request.push_back(0x00); /* Read Attributes */
    request.push_back(static_cast<unsigned char>(attribute));
    request.push_back(static_cast<unsigned char>(attribute >> 8));
    sendApsUnicast(destination, 0x0104, cluster, 1, endpoint, request);
}

void Gateway::requestStandardAttributes(unsigned short destination, unsigned char endpoint) {
    sendZclReadAttribute(destination, endpoint, 0x0001, 0x0021); /* battery percentage */
    sendZclReadAttribute(destination, endpoint, 0x0402, 0x0000); /* temperature */
    sendZclReadAttribute(destination, endpoint, 0x0405, 0x0000); /* humidity */
    sendZclReadAttribute(destination, endpoint, 0x0406, 0x0000); /* occupancy */
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
    {
        std::ostringstream diagnostic;
        diagnostic << "APS <- node=0x" << std::hex << sender << " profile=0x"
                   << (static_cast<unsigned short>(payload[1]) | (static_cast<unsigned short>(payload[2]) << 8))
                   << " cluster=0x" << cluster << " length=" << std::dec << length;
        addLog(diagnostic.str());
    }
    if (cluster == 0x0013 && message.size() >= 12) {
        /* ZDO Device_annce: transaction sequence, network address, EUI64, capability. */
        unsigned short announcedNode = static_cast<unsigned short>(message[1]) |
                                      (static_cast<unsigned short>(message[2]) << 8);
        registerNode(announcedNode, &message[3]);
        return;
    }
    if (cluster == 0x8005 && message.size() >= 5 && message[1] == 0) {
        unsigned long endpoint;
        unsigned long count = message[4];
        if (message.size() < 5 + count) return;
        for (endpoint = 0; endpoint < count; ++endpoint) {
            requestStandardAttributes(sender, message[5 + endpoint]);
        }
        addLog("Read standard attributes from announced device");
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
    } else if (command == 0x01) {
        std::vector<unsigned char> records(message.begin() + zclOffset, message.end());
        bool wasOpen = devices_[address->second].contactOpen;
        if (applyZclReadAttributeResponse(&devices_[address->second], cluster, records, time(0))) {
            updateContactAlert(address->second, wasOpen);
            saveState();
        }
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
