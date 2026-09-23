#include "gateway.h"
#include "json_util.h"
#include "notification_worker.h"
#include "storage.h"
#include "zcl.h"
#include "fcm.h"
#include "web_push.h"

#include <cerrno>
#include <algorithm>
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

EndpointDescriptor::EndpointDescriptor() : endpoint(0), profileId(0), deviceId(0), deviceVersion(0) {
}
SceneLightState::SceneLightState() : on(false), brightnessPercent(-1) {}
Scene::Scene() : id(0) {}
SceneGroup::SceneGroup() : id(0) {}
RemoteSceneBinding::RemoteSceneBinding() : groupId(0), off(false) {}
Gateway::SceneRetry::SceneRetry() : off(false), inFlight(false), retryRequested(false), sentOff(false), unicast(false), memberIndex(0), sceneId(0), sentSceneId(0),
                                    ezspSequence(0), attempts(0), sentAt(0), retryAt(0) {}

Device::Device() : networkAddress(0xffff), kind("generic"), contactOpen(false), hasContact(false), hasTemperature(false),
                   temperatureCelsius(0), hasHumidity(false), humidityPercent(0), hasOccupancy(false), occupied(false),
                   hasBattery(false), batteryPercent(0), hasLight(false), lightOn(false), hasBrightness(false),
                   brightnessPercent(0), hasRemote(false), linkQuality(0), lastSeen(0),
                   iasZoneId(-1), hasLowTemperatureAlert(false), hasHighTemperatureAlert(false), lowTemperatureAlert(0),
                   highTemperatureAlert(0), contactInitialAlertMinutes(10), contactReminderAlertMinutes(5),
                   sensorRefreshIntervalMinutes(5) {
}

Gateway::InterviewOperation::InterviewOperation() : type(InterviewNodeDescriptor), endpoint(0), cluster(0), attribute(0),
    dataType(0), minimumInterval(0), maximumInterval(0), reportableChange(0), transactionSequence(0),
    transactionSequenceAssigned(false), messageTag(0), attempts(0), retryAt(0) {
}

Gateway::InterviewState::InterviewState() : operationIndex(0), refreshOnly(false), paused(false), persistentDirty(false) {
}

namespace {
long sensorRefreshDeadline(long now, unsigned long minutes) {
    if (minutes == 0) return 0;
    if (now >= LONG_MAX || minutes > static_cast<unsigned long>((LONG_MAX - now) / 60)) return LONG_MAX;
    return now + static_cast<long>(minutes * 60);
}

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

void clearVolatileDeviceValues(Device* device) {
    device->contactOpen = false;
    device->temperatureCelsius = 0;
    device->humidityPercent = 0;
    device->occupied = false;
    device->hasBattery = false;
    device->batteryPercent = 0;
    device->lightOn = false;
    device->hasBrightness = false;
    device->brightnessPercent = 0;
    device->linkQuality = 0;
    device->lastSeen = 0;
}

bool endpointDescriptorsEqual(const std::vector<EndpointDescriptor>& left,
                              const std::vector<EndpointDescriptor>& right) {
    if (left.size() != right.size()) return false;
    for (unsigned long i = 0; i < left.size(); ++i) {
        if (left[i].endpoint != right[i].endpoint || left[i].profileId != right[i].profileId ||
            left[i].deviceId != right[i].deviceId || left[i].deviceVersion != right[i].deviceVersion ||
            left[i].inputClusters != right[i].inputClusters || left[i].outputClusters != right[i].outputClusters) return false;
    }
    return true;
}

std::string hexBytes(const std::vector<unsigned char>& bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned long i = 0; i < bytes.size(); ++i) output << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    return output.str();
}

const char* clusterName(unsigned short cluster) {
    switch (cluster) {
    case 0x0000: return "Basic";
    case 0x0001: return "Power Configuration";
    case 0x0002: return "Node Descriptor";
    case 0x0004: return "Simple Descriptor";
    case 0x0005: return "Active Endpoints";
    case 0x0021: return "Bind";
    case 0x0402: return "Temperature Measurement";
    case 0x0405: return "Relative Humidity";
    case 0x0406: return "Occupancy Sensing";
    case 0x0500: return "IAS Zone";
    case 0x0013: return "Device Announcement";
    case 0x8002: return "Node Descriptor Response";
    case 0x8004: return "Simple Descriptor Response";
    case 0x8005: return "Active Endpoints Response";
    case 0x8021: return "Bind Response";
    default: return "";
    }
}

const char* zclCommandName(unsigned char command) {
    switch (command) {
    case 0x00: return "Read Attributes";
    case 0x01: return "Read Attributes Response";
    case 0x06: return "Configure Reporting";
    case 0x07: return "Configure Reporting Response";
    case 0x0a: return "Report Attributes";
    default: return "";
    }
}

std::string knownAttributeValue(unsigned short cluster, unsigned short attribute, const std::vector<unsigned char>& message,
                                unsigned long offset) {
    std::ostringstream out;
    if (cluster == 0x0402 && attribute == 0x0000 && offset + 2 <= message.size()) {
        short raw = static_cast<short>(static_cast<unsigned short>(message[offset]) |
                                       (static_cast<unsigned short>(message[offset + 1]) << 8));
        out << " value=" << std::fixed << std::setprecision(2) << (static_cast<double>(raw) / 100.0) << "C";
    } else if (cluster == 0x0405 && attribute == 0x0000 && offset + 2 <= message.size()) {
        unsigned short raw = static_cast<unsigned short>(message[offset]) | (static_cast<unsigned short>(message[offset + 1]) << 8);
        out << " value=" << std::fixed << std::setprecision(2) << (static_cast<double>(raw) / 100.0) << "%";
    } else if (cluster == 0x0406 && attribute == 0x0000 && offset < message.size()) {
        out << " value=" << ((message[offset] & 1) ? "occupied" : "clear");
    } else if (cluster == 0x0001 && attribute == 0x0021 && offset < message.size()) {
        out << " value=" << (static_cast<unsigned int>(message[offset]) / 2) << "%";
    } else if (cluster == 0x0500 && attribute == 0x0002 && offset + 2 <= message.size()) {
        out << " value=" << ((message[offset] & 1) ? "open" : "closed");
    }
    return out.str();
}

std::string decodeFramePayload(unsigned short profile, unsigned short cluster, const std::vector<unsigned char>& message) {
    std::ostringstream out;
    if (message.empty()) return " empty";
    if (profile == 0x0000) {
        out << " zdo_seq=" << std::dec << static_cast<unsigned int>(message[0]);
        if ((cluster == 0x0013) && message.size() >= 12) {
            unsigned short node = static_cast<unsigned short>(message[1]) | (static_cast<unsigned short>(message[2]) << 8);
            out << " announced_node=0x" << std::hex << node << " eui64=0x" << std::setfill('0');
            for (int i = 10; i >= 3; --i) out << std::setw(2) << static_cast<unsigned int>(message[i]);
            out << " capability=0x" << static_cast<unsigned int>(message[11]);
        } else if ((cluster & 0x8000) != 0 && message.size() >= 2) {
            out << " status=0x" << std::hex << static_cast<unsigned int>(message[1]);
            if (cluster == 0x8005 && message.size() >= 5) {
                out << " node=0x" << (static_cast<unsigned short>(message[2]) | (static_cast<unsigned short>(message[3]) << 8))
                    << " endpoints=" << std::dec << static_cast<unsigned int>(message[4]);
            }
        }
        return out.str();
    }
    if (message.size() < 3) return " zcl=truncated";
    unsigned char control = message[0];
    unsigned long offset = 1;
    out << " zcl_control=0x" << std::hex << static_cast<unsigned int>(control);
    if ((control & 0x04) != 0) {
        if (message.size() < 5) return out.str() + " manufacturer=truncated";
        out << " manufacturer=0x" << (static_cast<unsigned short>(message[offset]) | (static_cast<unsigned short>(message[offset + 1]) << 8));
        offset += 2;
    }
    if (offset + 2 > message.size()) return out.str() + " zcl=truncated";
    unsigned char transaction = message[offset++];
    unsigned char command = message[offset++];
    out << " transaction=" << std::dec << static_cast<unsigned int>(transaction) << " command=0x" << std::hex
        << static_cast<unsigned int>(command);
    if (zclCommandName(command)[0] != '\0') out << "(" << zclCommandName(command) << ")";
    if ((command == 0x00 || command == 0x06) && offset + 2 <= message.size()) {
        unsigned short attribute = static_cast<unsigned short>(message[offset]) | (static_cast<unsigned short>(message[offset + 1]) << 8);
        out << " attribute=0x" << attribute;
        if (command == 0x06 && offset + 7 <= message.size()) {
            unsigned short minimum = static_cast<unsigned short>(message[offset + 3]) |
                                     (static_cast<unsigned short>(message[offset + 4]) << 8);
            unsigned short maximum = static_cast<unsigned short>(message[offset + 5]) |
                                     (static_cast<unsigned short>(message[offset + 6]) << 8);
            out << " type=0x" << static_cast<unsigned int>(message[offset + 2])
                << " min=" << std::dec << minimum << "s max=" << maximum << "s";
        }
    }
    if (command == 0x0a && offset + 3 <= message.size()) {
        unsigned short attribute = static_cast<unsigned short>(message[offset]) | (static_cast<unsigned short>(message[offset + 1]) << 8);
        out << " attribute=0x" << attribute << " type=0x" << static_cast<unsigned int>(message[offset + 2]);
        out << knownAttributeValue(cluster, attribute, message, offset + 3);
    }
    if (command == 0x01 && offset + 4 <= message.size()) {
        unsigned short attribute = static_cast<unsigned short>(message[offset]) | (static_cast<unsigned short>(message[offset + 1]) << 8);
        unsigned char status = message[offset + 2];
        out << " attribute=0x" << attribute << " status=0x" << static_cast<unsigned int>(status);
        if (status == 0) {
            out << " type=0x" << static_cast<unsigned int>(message[offset + 3]);
            out << knownAttributeValue(cluster, attribute, message, offset + 4);
        }
    }
    if (cluster == 0x0500 && command == 0x00 && offset + 2 <= message.size()) {
        unsigned short zoneStatus = static_cast<unsigned short>(message[offset]) |
                                    (static_cast<unsigned short>(message[offset + 1]) << 8);
        out << " zone_status=0x" << std::hex << zoneStatus << " contact=" << ((zoneStatus & 1) ? "open" : "closed");
    }
    return out.str();
}
}

Gateway::Gateway(const std::string& serialDevice, int serialBaud, const std::string& runtimeDirectory,
                 const std::string& firebaseServiceAccount, const std::string& firebaseTopic, const std::string& caBundle,
                 NotificationWorker* notificationWorker) :
    serialDevice_(serialDevice), serialBaud_(serialBaud), runtimeDirectory_(runtimeDirectory),
    firebaseServiceAccount_(firebaseServiceAccount), firebaseTopic_(firebaseTopic), caBundle_(caBundle), notificationWorker_(notificationWorker), serialFd_(-1),
    online_(false), joinUntil_(0), joinPermitRenewAt_(0), pendingJoinSeconds_(0), joinPreparationStep_(0), joinPreparationDeadline_(0), ashSendSequence_(0), ashReceiveSequence_(0), ezspSequence_(0),
    ashAckDeadline_(0), ashRetries_(0), ezspVersion_(4), zdoSequence_(0), zclTransactionSequence_(0), ezspVersionNegotiated_(false), stackTokenWritingRequested_(false), networkInitRequested_(false), coordinatorInitializationStep_(0), resetDeadline_(0), nextHeartbeatAt_(0), serialRxBytes_(0), ashRxFrames_(0), networkForming_(false), networkFormationStep_(0), networkFormed_(false), networkPanId_(0),
    networkChannel_(0), interviewRequestInFlight_(false), interviewSendAccepted_(false), interviewEzspSequence_(0), interviewNode_(0xffff),
    interviewDeadline_(0), nextMessageTag_(1), coordinatorIeeeAddressRequested_(false), viewDirty_(true) {
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
    /* With O_NONBLOCK, a zero-byte minimum makes select() report the TTY as
       readable even when read() would return no data.  One byte keeps the
       coordinator asleep until serial input is actually available. */
    options.c_cc[VMIN] = 1;
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
    pollAshTransmit();
    pollSceneRetries();
    if (online_ && nextHeartbeatAt_ != 0 && time(0) >= nextHeartbeatAt_) {
        addLog("Coordinator heartbeat");
        sendEzspCommand(0x0005, std::vector<unsigned char>()); /* nop */
        nextHeartbeatAt_ = time(0) + 20;
    }
    if (!online_ && resetDeadline_ != 0 && time(0) >= resetDeadline_) sendAshReset();
    if (interviewRequestInFlight_ && interviewDeadline_ != 0 && time(0) >= interviewDeadline_) {
        failInterviewOperation(interviewNode_, "response timed out");
    }
    if (online_ && networkFormed_ && coordinatorInitializationStep_ == 0 && !interviewRequestInFlight_) {
        sendNextInterviewOperation();
    }
    if (online_ && networkFormed_ && coordinatorInitializationStep_ == 0 && !interviewRequestInFlight_ &&
        !lightSetupPending_.empty()) {
        std::map<unsigned short, bool>::iterator pending = lightSetupPending_.begin();
        unsigned short node = pending->first;
        lightSetupPending_.erase(pending);
        if (nodeAddresses_.find(node) != nodeAddresses_.end()) {
            startInterview(node, false);
            addLog("Queued light reporting setup for existing device");
        }
    }
    if (online_ && networkFormed_ && coordinatorInitializationStep_ == 0 && !interviewRequestInFlight_) {
        std::map<unsigned short, std::string>::const_iterator node;
        long now = time(0);
        for (node = nodeAddresses_.begin(); node != nodeAddresses_.end(); ++node) {
            std::map<std::string, Device>::const_iterator device = devices_.find(node->second);
            bool measurement = false;
            if (device == devices_.end()) continue;
            for (unsigned long endpoint = 0; endpoint < device->second.endpoints.size(); ++endpoint) {
                if (endpointSupports(device->second.endpoints[endpoint], 0x0402) ||
                    endpointSupports(device->second.endpoints[endpoint], 0x0405)) measurement = true;
            }
            if (!measurement && !device->second.hasTemperature && !device->second.hasHumidity) continue;
            if (device->second.sensorRefreshIntervalMinutes == 0) continue;
            if (nextSensorRefreshAt_[node->first] > now) continue;
            startInterview(node->first, true);
            nextSensorRefreshAt_[node->first] = sensorRefreshDeadline(now, device->second.sensorRefreshIntervalMinutes);
            addLog("Queued periodic sensor discovery refresh");
            break; /* one sleepy-device request at a time */
        }
    }
    if (joinPreparationStep_ != 0 && joinPreparationDeadline_ != 0 && time(0) >= joinPreparationDeadline_) {
        addLog("Secure pairing preparation timed out; try allowing joining again");
        joinPreparationStep_ = 0;
        joinPreparationDeadline_ = 0;
        pendingJoinSeconds_ = 0;
    }
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

int Gateway::serialFd() const { return serialFd_; }
bool Gateway::serialNeedsWrite() const { return !serialPendingBytes_.empty(); }
bool Gateway::hasImmediateWork() const {
    return serialPendingBytes_.empty() && ashPendingData_.empty() && !ashTransmitQueue_.empty();
}

long Gateway::nextPollDeadline() const {
    long deadline = 0;
    long now = time(0);
    std::map<unsigned short, long>::const_iterator sensor;
    std::map<std::string, long>::const_iterator alert;
    std::map<unsigned short, SceneRetry>::const_iterator scene;
    std::map<unsigned short, InterviewState>::const_iterator interview;
    #define CONSIDER_DEADLINE(value) do { long candidate = (value); if (candidate != 0 && (deadline == 0 || candidate < deadline)) deadline = candidate; } while (0)
    if (!ashPendingData_.empty()) CONSIDER_DEADLINE(ashAckDeadline_);
    if (online_) CONSIDER_DEADLINE(nextHeartbeatAt_);
    if (!online_) CONSIDER_DEADLINE(resetDeadline_);
    if (interviewRequestInFlight_) CONSIDER_DEADLINE(interviewDeadline_);
    CONSIDER_DEADLINE(joinPreparationDeadline_);
    CONSIDER_DEADLINE(joinUntil_);
    if (joinUntil_ != 0) CONSIDER_DEADLINE(joinPermitRenewAt_);
    for (alert = nextContactAlertAt_.begin(); alert != nextContactAlertAt_.end(); ++alert) CONSIDER_DEADLINE(alert->second);
    if (online_ && networkFormed_ && coordinatorInitializationStep_ == 0 && !interviewRequestInFlight_) {
        for (interview = interviews_.begin(); interview != interviews_.end(); ++interview) {
            const InterviewState& state = interview->second;
            if (!state.paused && state.operationIndex < state.operations.size())
                CONSIDER_DEADLINE(state.operations[state.operationIndex].retryAt);
        }
        for (sensor = nextSensorRefreshAt_.begin(); sensor != nextSensorRefreshAt_.end(); ++sensor) CONSIDER_DEADLINE(sensor->second);
        for (scene = sceneRetries_.begin(); scene != sceneRetries_.end(); ++scene)
            CONSIDER_DEADLINE(scene->second.inFlight ? scene->second.sentAt + 2 : scene->second.retryAt);
    }
    #undef CONSIDER_DEADLINE
    return deadline < now ? now : deadline;
}

bool Gateway::setJoining(unsigned int seconds, std::string* error) {
    if (!online_) {
        *error = "Zigbee coordinator is offline";
        addLog("Pairing request rejected: coordinator offline");
        return false;
    }
    if (!networkFormed_ || coordinatorInitializationStep_ != 0) {
        *error = "Zigbee network is not ready";
        addLog("Pairing request rejected: Zigbee network not ready");
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
        joinPreparationDeadline_ = 0;
        addLog("Pairing closed");
    } else if (ezspVersion_ >= 8) {
        pendingJoinSeconds_ = seconds;
        joinPreparationStep_ = 1;
        joinPreparationDeadline_ = time(0) + 10;
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
    if (!networkInitRequested_ || coordinatorInitializationStep_ != 0) {
        *error = "Zigbee coordinator is still starting";
        return false;
    }
    if (networkForming_) { *error = "network creation is already in progress"; return false; }
    if (!generateNetworkMaterial(error)) return false;
    networkForming_ = true;
    leaveExisting = networkFormed_;
    networkFormed_ = false;
    coordinatorIeeeAddress_.clear();
    coordinatorIeeeAddressRequested_ = false;
    interviews_.clear();
    interviewRequestInFlight_ = false;
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
                                unsigned long reminderAlertMinutes, bool hasSensorRefreshInterval,
                                unsigned long sensorRefreshIntervalMinutes, std::string* error) {
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
    if (hasSensorRefreshInterval && !it->second.hasTemperature && !it->second.hasHumidity) {
        *error = "sensor refresh interval applies only to measurement devices";
        return false;
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
    if (hasSensorRefreshInterval) {
        it->second.sensorRefreshIntervalMinutes = sensorRefreshIntervalMinutes;
        if (sensorRefreshIntervalMinutes == 0) nextSensorRefreshAt_.erase(it->second.networkAddress);
        else nextSensorRefreshAt_[it->second.networkAddress] = sensorRefreshDeadline(time(0), sensorRefreshIntervalMinutes);
    }
    saveState();
    savePersistentState();
    return true;
}

bool Gateway::refreshDevice(const std::string& ieeeAddress, std::string* error) {
    std::map<std::string, Device>::const_iterator device = devices_.find(ieeeAddress);
    if (device == devices_.end()) { *error = "device is unknown"; return false; }
    if (!online_ || !networkFormed_ || coordinatorInitializationStep_ != 0) {
        *error = "Zigbee network is not ready";
        addLog(std::string("Manual refresh rejected; Zigbee network not ready: ") + ieeeAddress);
        return false;
    }
    if (device->second.networkAddress == 0xffff) { *error = "device has no active network address"; return false; }
    /* A remote needs a full interview so its output command clusters are
       bound to the coordinator.  Measurement devices only need their
       persisted endpoints refreshed. */
    startInterview(device->second.networkAddress, !device->second.hasRemote);
    if (device->second.sensorRefreshIntervalMinutes == 0) nextSensorRefreshAt_.erase(device->second.networkAddress);
    else nextSensorRefreshAt_[device->second.networkAddress] =
        sensorRefreshDeadline(time(0), device->second.sensorRefreshIntervalMinutes);
    addLog(std::string(device->second.hasRemote ? "Manual remote configuration queued: " :
                       "Manual attribute refresh queued: ") + ieeeAddress);
    return true;
}

bool Gateway::endpointForCluster(const Device& device, unsigned short cluster, unsigned char* endpoint) const {
    for (unsigned long i = 0; i < device.endpoints.size(); ++i) {
        if (endpointSupports(device.endpoints[i], cluster)) { *endpoint = device.endpoints[i].endpoint; return true; }
    }
    return false;
}

void Gateway::sendZclLightCommand(unsigned short destination, unsigned char endpoint, unsigned short cluster,
                                  unsigned char command, const std::vector<unsigned char>& arguments) {
    std::vector<unsigned char> message;
    message.push_back(0x11);
    message.push_back(++zclTransactionSequence_);
    message.push_back(command);
    message.insert(message.end(), arguments.begin(), arguments.end());
    sendApsUnicast(destination, 0x0104, cluster, 1, endpoint, message);
}

bool Gateway::setLightState(const std::string& ieeeAddress, const std::string& state, int brightnessPercent,
                            std::string* error) {
    std::map<std::string, Device>::iterator found = devices_.find(ieeeAddress);
    unsigned char endpoint;
    if (found == devices_.end() || !found->second.hasLight) { *error = "device is not a light"; return false; }
    if (!online_ || !networkFormed_ || coordinatorInitializationStep_ != 0) { *error = "Zigbee network is not ready"; return false; }
    if (found->second.networkAddress == 0xffff) { *error = "device has no active network address"; return false; }
    if (brightnessPercent >= 0) {
        if (brightnessPercent > 100 || !endpointForCluster(found->second, 0x0008, &endpoint)) {
            *error = "brightness is unavailable for this light"; return false;
        }
        std::vector<unsigned char> arguments;
        unsigned int level = static_cast<unsigned int>((brightnessPercent * 254 + 50) / 100);
        arguments.push_back(static_cast<unsigned char>(level)); arguments.push_back(0); arguments.push_back(0);
        sendZclLightCommand(found->second.networkAddress, endpoint, 0x0008, 0x04, arguments);
        found->second.hasBrightness = true;
        found->second.brightnessPercent = brightnessPercent;
        found->second.lightOn = brightnessPercent != 0;
        saveState();
        addLog(std::string("Light brightness command queued: ") + ieeeAddress);
        return true;
    }
    unsigned char command;
    if (!endpointForCluster(found->second, 0x0006, &endpoint)) { *error = "power control is unavailable for this light"; return false; }
    if (state == "off") command = 0x00;
    else if (state == "on") command = 0x01;
    else if (state == "toggle") command = 0x02;
    else { *error = "invalid light state"; return false; }
    sendZclLightCommand(found->second.networkAddress, endpoint, 0x0006, command, std::vector<unsigned char>());
    if (state == "toggle") found->second.lightOn = !found->second.lightOn;
    else found->second.lightOn = state == "on";
    saveState();
    addLog(std::string("Light power command queued: ") + ieeeAddress + " " + state);
    return true;
}

SceneGroup* Gateway::findSceneGroup(unsigned short id) {
    for (unsigned long i = 0; i < sceneGroups_.size(); ++i) if (sceneGroups_[i].id == id) return &sceneGroups_[i];
    return 0;
}
const SceneGroup* Gateway::findSceneGroup(unsigned short id) const {
    for (unsigned long i = 0; i < sceneGroups_.size(); ++i) if (sceneGroups_[i].id == id) return &sceneGroups_[i];
    return 0;
}

bool Gateway::sceneMemberNative(const Device& device, const Scene&, const SceneLightState& state) const {
    unsigned char endpoint;
    if (!endpointForCluster(device, 0x0004, &endpoint) || !endpointForCluster(device, 0x0005, &endpoint) ||
        !endpointForCluster(device, 0x0006, &endpoint)) return false;
    return state.brightnessPercent < 0 || endpointForCluster(device, 0x0008, &endpoint);
}

void Gateway::provisionSceneGroup(const SceneGroup& group) {
    for (unsigned long i = 0; i < group.members.size(); ++i) {
        std::map<std::string, Device>::const_iterator found = devices_.find(group.members[i]);
        unsigned char endpoint;
        if (found == devices_.end() || found->second.networkAddress == 0xffff ||
            !endpointForCluster(found->second, 0x0004, &endpoint)) continue;
        std::vector<unsigned char> add;
        add.push_back(0x11); add.push_back(++zclTransactionSequence_); add.push_back(0x00);
        add.push_back(static_cast<unsigned char>(group.id)); add.push_back(static_cast<unsigned char>(group.id >> 8));
        add.push_back(static_cast<unsigned char>(group.name.size())); add.insert(add.end(), group.name.begin(), group.name.end());
        sendApsUnicast(found->second.networkAddress, 0x0104, 0x0004, 1, endpoint, add);
        for (unsigned long j = 0; j < group.scenes.size(); ++j) {
            const Scene& scene = group.scenes[j];
            const SceneLightState* state = 0;
            for (unsigned long k = 0; k < scene.states.size(); ++k)
                if (scene.states[k].ieeeAddress == group.members[i]) { state = &scene.states[k]; break; }
            if (state == 0 || !sceneMemberNative(found->second, scene, *state)) continue;
            std::vector<unsigned char> command;
            command.push_back(0x11); command.push_back(++zclTransactionSequence_); command.push_back(0x00);
            command.push_back(static_cast<unsigned char>(group.id)); command.push_back(static_cast<unsigned char>(group.id >> 8));
            command.push_back(scene.id); command.push_back(0); command.push_back(0); /* transition */
            command.push_back(static_cast<unsigned char>(scene.name.size())); command.insert(command.end(), scene.name.begin(), scene.name.end());
            command.push_back(0x06); command.push_back(0); command.push_back(1); command.push_back(state->on ? 1 : 0);
            if (state->brightnessPercent >= 0) { command.push_back(0x08); command.push_back(0); command.push_back(1); command.push_back(static_cast<unsigned char>((state->brightnessPercent * 254 + 50) / 100)); }
            sendApsUnicast(found->second.networkAddress, 0x0104, 0x0005, 1, endpoint, command);
        }
    }
}

bool Gateway::activateScene(unsigned short groupId, unsigned char sceneId, std::string* error) {
    SceneGroup* group = findSceneGroup(groupId);
    if (group == 0) { *error = "scene group not found"; return false; }
    bool found = false;
    for (unsigned long i = 0; i < group->scenes.size(); ++i) if (group->scenes[i].id == sceneId) found = true;
    if (!found) { *error = "scene not found"; return false; }
    if (!online_ || !networkFormed_ || coordinatorInitializationStep_ != 0) { *error = "Zigbee network is not ready"; return false; }
    SceneRetry& retry = sceneRetries_[groupId];
    retry.off = false; retry.sceneId = sceneId; retry.attempts = 0; retry.retryRequested = false; retry.retryAt = time(0);
    if (!retry.inFlight) { retry.unicast = false; retry.memberIndex = 0; }
    if (!retry.inFlight) pollSceneRetries();
    return true;
}

bool Gateway::turnOffSceneGroup(unsigned short groupId, std::string* error) {
    if (findSceneGroup(groupId) == 0) { *error = "scene group not found"; return false; }
    if (!online_ || !networkFormed_ || coordinatorInitializationStep_ != 0) { *error = "Zigbee network is not ready"; return false; }
    SceneRetry& retry = sceneRetries_[groupId];
    retry.off = true; retry.sceneId = 0; retry.attempts = 0; retry.retryRequested = false; retry.retryAt = time(0);
    if (!retry.inFlight) { retry.unicast = false; retry.memberIndex = 0; }
    if (!retry.inFlight) pollSceneRetries();
    return true;
}

bool Gateway::sendSceneRetry(unsigned short groupId, SceneRetry* retry) {
    SceneGroup* group = findSceneGroup(groupId);
    Scene* scene = 0;
    bool multicast = false;
    if (group == 0) return false;
    if (!retry->off) for (unsigned long i=0;i<group->scenes.size();++i) if(group->scenes[i].id==retry->sceneId) scene=&group->scenes[i];
    if (!retry->off && scene == 0) return false;
    if (retry->off) {
        for (unsigned long i=0;i<group->members.size();++i) { std::map<std::string,Device>::const_iterator d=devices_.find(group->members[i]); unsigned char endpoint; if(d!=devices_.end() && endpointForCluster(d->second,0x0004,&endpoint) && endpointForCluster(d->second,0x0006,&endpoint)) multicast=true; }
    } else for (unsigned long i=0;i<scene->states.size();++i) {
        std::map<std::string, Device>::const_iterator d = devices_.find(scene->states[i].ieeeAddress);
        if (d == devices_.end() || d->second.networkAddress == 0xffff) { addLog("Scene retry waiting for unavailable member"); return false; }
        if (sceneMemberNative(d->second, *scene, scene->states[i])) multicast = true;
    }
    if (!multicast) { completeSceneRetry(groupId, retry); retry->retryAt=LONG_MAX; return true; }
    std::vector<unsigned char> message;
    message.push_back(0x11); message.push_back(++zclTransactionSequence_);
    if (retry->off) message.push_back(0x00);
    else { message.push_back(0x05); message.push_back(static_cast<unsigned char>(groupId)); message.push_back(static_cast<unsigned char>(groupId >> 8)); message.push_back(retry->sceneId); }
    retry->sentOff=retry->off; retry->sentSceneId=retry->sceneId;
    if (retry->unicast) {
        /* Serialize native recalls so a full broadcast table does not block
           user actions or cause a burst of unicast requests. */
        for (; retry->memberIndex < group->members.size(); ++retry->memberIndex) {
            std::map<std::string, Device>::const_iterator d = devices_.find(group->members[retry->memberIndex]);
            if (d == devices_.end()) return false;
            unsigned char endpoint;
            bool native = false;
            if (retry->off) native = endpointForCluster(d->second, 0x0004, &endpoint);
            else for (unsigned long i = 0; i < scene->states.size(); ++i)
                if (scene->states[i].ieeeAddress == d->first)
                    native = sceneMemberNative(d->second, *scene, scene->states[i]);
            if (!native) continue; /* handled by completeSceneRetry */
            if (d->second.networkAddress == 0xffff ||
                !endpointForCluster(d->second, retry->off ? 0x0006 : 0x0005, &endpoint)) return false;
            retry->ezspSequence = sendApsUnicast(d->second.networkAddress, 0x0104,
                retry->off ? 0x0006 : 0x0005, 1, endpoint, message);
            retry->inFlight = true; retry->sentAt = time(0);
            return true;
        }
        completeSceneRetry(groupId, retry); retry->retryAt = LONG_MAX;
        return true;
    }
    retry->ezspSequence=sendApsMulticast(groupId,0x0104,retry->off?0x0006:0x0005,1,message);
    retry->inFlight=true; retry->sentAt=time(0);
    return true;
}

void Gateway::completeSceneRetry(unsigned short groupId, SceneRetry* retry) {
    SceneGroup* group=findSceneGroup(groupId);
    if (group==0) return;
    if (retry->off) {
        for(unsigned long i=0;i<group->members.size();++i) { std::map<std::string,Device>::const_iterator d=devices_.find(group->members[i]); unsigned char e; if(d!=devices_.end() && !endpointForCluster(d->second,0x0004,&e)) { std::string ignored; setLightState(group->members[i],"off",-1,&ignored); } }
        sceneGroupOff_[groupId]=true; addLog(std::string("Scene group off: ")+group->name);
    } else {
        Scene* scene=0; for(unsigned long i=0;i<group->scenes.size();++i)if(group->scenes[i].id==retry->sceneId)scene=&group->scenes[i];
        if(scene==0)return;
        for(unsigned long i=0;i<scene->states.size();++i) { std::map<std::string,Device>::const_iterator d=devices_.find(scene->states[i].ieeeAddress); if(d!=devices_.end() && !sceneMemberNative(d->second,*scene,scene->states[i])) { std::string ignored; setLightState(scene->states[i].ieeeAddress,scene->states[i].on?"on":"off",scene->states[i].on?scene->states[i].brightnessPercent:-1,&ignored); } }
        selectedScenes_[groupId]=retry->sceneId; sceneGroupOff_[groupId]=false;
        addLog(std::string("Scene recalled: ")+group->name+" / "+scene->name);
    }
    saveSceneRuntime();
}

bool Gateway::scheduleSceneRetry(SceneRetry* retry, long now) {
    /* Retry once per second for the first five seconds, then make one final
       attempt ten seconds later.  Busy is usually a brief NCP TX-queue
       condition, so exponential backoff needlessly delays scene selection. */
    if (retry->attempts < 5) {
        ++retry->attempts;
        retry->retryAt = now + 1;
        return true;
    }
    if (retry->attempts == 5) {
        ++retry->attempts;
        retry->retryAt = now + 10;
        return true;
    }
    retry->retryAt = LONG_MAX;
    return false;
}

void Gateway::pollSceneRetries() {
    if (!online_ || !networkFormed_ || coordinatorInitializationStep_ != 0) return;
    long now=time(0);
    for(std::map<unsigned short,SceneRetry>::iterator it=sceneRetries_.begin();it!=sceneRetries_.end();++it) {
        SceneRetry& retry=it->second;
        if (retry.inFlight && now >= retry.sentAt + 2) {
            retry.inFlight=false;
            if (retry.sentOff != retry.off || (!retry.off && retry.sentSceneId != retry.sceneId)) {
                retry.unicast=false; retry.memberIndex=0;
                retry.retryRequested=true;
            }
            if (retry.retryRequested) {
                retry.retryRequested=false;
                retry.retryAt=now;
                addLog("Scene multicast response timed out; retry requested, retrying immediately");
            } else if (scheduleSceneRetry(&retry, now)) addLog("Scene multicast response timed out; retrying");
            else addLog("Scene multicast response timed out; giving up");
        }
        if(!retry.inFlight && retry.retryAt<=now && !sendSceneRetry(it->first,&retry)) retry.retryAt=now+5;
    }
}

void Gateway::dispatchRemoteSceneAction(const std::string& ieeeAddress, const std::string& action) {
    for (unsigned long i=0;i<sceneBindings_.size();++i) if (sceneBindings_[i].ieeeAddress==ieeeAddress && sceneBindings_[i].action==action) {
        RemoteSceneBinding& binding=sceneBindings_[i]; std::string error;
        if (binding.off) { turnOffSceneGroup(binding.groupId,&error); return; }
        if (binding.sceneIds.empty()) return;
        unsigned char wanted=binding.sceneIds[0]; unsigned char current=0;
        std::map<unsigned short,SceneRetry>::iterator retry=sceneRetries_.find(binding.groupId);
        if (retry != sceneRetries_.end() &&
            (retry->second.inFlight || retry->second.retryAt != LONG_MAX)) {
            if (retry->second.inFlight) {
                retry->second.retryRequested=true;
                addLog("Scene selection pending; immediate retry requested");
            } else {
                retry->second.retryAt=time(0);
                retry->second.retryRequested=false;
                addLog("Scene selection pending; retrying immediately");
                pollSceneRetries();
            }
            return;
        }
        if(retry!=sceneRetries_.end() && !retry->second.off) current=retry->second.sceneId;
        else if(selectedScenes_.count(binding.groupId)) current=selectedScenes_[binding.groupId];
        if(current!=0) for(unsigned long j=0;j<binding.sceneIds.size();++j) if(binding.sceneIds[j]==current) {
            if (sceneGroupOff_[binding.groupId] && (retry==sceneRetries_.end() || retry->second.off)) wanted=current;
            else wanted=binding.sceneIds[(j+1)%binding.sceneIds.size()];
            break;
        }
        if (activateScene(binding.groupId,wanted,&error)) { }
        else addLog(std::string("Scene binding failed: ")+error);
        return;
    }
}

bool Gateway::setSceneConfiguration(const std::string& encoded, std::string* error) {
    const std::string incoming = "/tmp/microhad-zigbee-scenes-import";
    std::ofstream output(incoming.c_str());
    std::vector<SceneGroup> groups;
    std::vector<RemoteSceneBinding> bindings;
    if (!output) { *error = "cannot stage scene configuration"; return false; }
    output << encoded; output.close();
    if (!loadSceneConfiguration(incoming, &groups, &bindings, error)) { unlink(incoming.c_str()); return false; }
    unlink(incoming.c_str());
    for (unsigned long i=0;i<groups.size();++i) {
        if (groups[i].id == 0 || groups[i].id > 0xfff7 || groups[i].name.empty() || groups[i].members.empty()) { *error="invalid scene group"; return false; }
        for (unsigned long j=0;j<i;++j) if (groups[j].id==groups[i].id) { *error="duplicate scene group"; return false; }
        for (unsigned long j=0;j<groups[i].members.size();++j) {
            if (devices_.find(groups[i].members[j])==devices_.end() || !devices_[groups[i].members[j]].hasLight) { *error="group member is not a light"; return false; }
            for (unsigned long a=0;a<=i;++a) for (unsigned long b=0;b<groups[a].members.size();++b)
                if ((a!=i || b!=j) && groups[a].members[b]==groups[i].members[j]) { *error="a light may belong to only one scene group"; return false; }
        }
        for (unsigned long j=0;j<groups[i].scenes.size();++j) {
            Scene& scene=groups[i].scenes[j];
            if (scene.id==0 || scene.name.empty() || scene.states.size()!=groups[i].members.size()) { *error="scene must define every group light"; return false; }
            for (unsigned long k=0;k<scene.states.size();++k) {
                if (std::find(groups[i].members.begin(),groups[i].members.end(),scene.states[k].ieeeAddress)==groups[i].members.end() ||
                    scene.states[k].brightnessPercent < -1 || scene.states[k].brightnessPercent > 100) { *error="invalid scene light state"; return false; }
                for (unsigned long previous=0; previous<k; ++previous)
                    if (scene.states[previous].ieeeAddress == scene.states[k].ieeeAddress) { *error="scene contains a duplicate light state"; return false; }
            }
        }
    }
    for (unsigned long i=0;i<bindings.size();++i) {
        const SceneGroup* group=findSceneGroup(bindings[i].groupId); /* old configuration is irrelevant */
        group=0; for (unsigned long j=0;j<groups.size();++j) if(groups[j].id==bindings[i].groupId) group=&groups[j];
        if (group==0 || (bindings[i].action!="on" && bindings[i].action!="off" && bindings[i].action!="arrow_left" && bindings[i].action!="arrow_right") ||
            (!bindings[i].off && bindings[i].sceneIds.empty())) { *error="invalid remote binding"; return false; }
    }
    sceneGroups_=groups; sceneBindings_=bindings; saveScenes();
    for (unsigned long i=0;i<sceneGroups_.size();++i) provisionSceneGroup(sceneGroups_[i]);
    addLog("Scene configuration saved; native provisioning queued");
    return true;
}

void Gateway::classifyDevice(Device* device) const {
    bool onOffServer = false, levelServer = false, remote = false;
    for (unsigned long i = 0; i < device->endpoints.size(); ++i) {
        const EndpointDescriptor& endpoint = device->endpoints[i];
        if (endpointSupports(endpoint, 0x0006)) onOffServer = true;
        if (endpointSupports(endpoint, 0x0008)) levelServer = true;
        if (std::find(endpoint.outputClusters.begin(), endpoint.outputClusters.end(), 0x0006) != endpoint.outputClusters.end() ||
            std::find(endpoint.outputClusters.begin(), endpoint.outputClusters.end(), 0x0008) != endpoint.outputClusters.end()) remote = true;
    }
    device->hasLight = onOffServer;
    device->hasRemote = remote && !onOffServer;
    if (device->hasLight) device->kind = levelServer ? "dimmable light" : "light";
    else if (device->hasRemote) device->kind = "remote control";
}

bool Gateway::logRemoteKeyEvent(unsigned short sender, unsigned short cluster, unsigned char endpoint,
                                unsigned short manufacturer, unsigned char command, const std::vector<unsigned char>& arguments) {
    std::map<unsigned short, std::string>::const_iterator address = nodeAddresses_.find(sender);
    if (address == nodeAddresses_.end()) return false;
    std::map<std::string, Device>::const_iterator device = devices_.find(address->second);
    if (device == devices_.end() || !device->second.hasRemote) return false;
    bool advertisesCluster = false;
    for (unsigned long i = 0; i < device->second.endpoints.size(); ++i) {
        const EndpointDescriptor& descriptor = device->second.endpoints[i];
        if (descriptor.endpoint == endpoint &&
            std::find(descriptor.outputClusters.begin(), descriptor.outputClusters.end(), cluster) != descriptor.outputClusters.end()) {
            advertisesCluster = true; break;
        }
    }
    /* STYRBAR uses IKEA manufacturer-specific Scenes commands even though
       its descriptor does not list the Scenes cluster as an output cluster. */
    bool styrbarSceneCommand = cluster == 0x0005 && manufacturer == 0x117c &&
        ((command == 0x07 && arguments.size() >= 4) ||
         (command == 0x08 && arguments.size() >= 3) || command == 0x09);
    if (!advertisesCluster && !styrbarSceneCommand) return false;
    const char* action = 0;
    std::string releaseAction;
    unsigned short sceneGroup = 0;
    unsigned char sceneId = 0;
    if (cluster == 0x0006) {
        if (command == 0x00) action = "off";
        else if (command == 0x01) action = "on";
        else if (command == 0x02) action = "toggle";
    } else if (cluster == 0x0008) {
        if (command == 0x00 || command == 0x04) action = "move-to-level";
        else if (command == 0x01 || command == 0x05) action = "move";
        else if (command == 0x02 || command == 0x06) action = "step";
        else if (command == 0x03 || command == 0x07) action = "stop";
    } else if (styrbarSceneCommand) {
        if (command == 0x07) {
            sceneGroup = static_cast<unsigned short>(arguments[0]) | (static_cast<unsigned short>(arguments[1]) << 8);
            sceneId = arguments[2];
            if (sceneGroup == 0x0101 && sceneId == 13) action = "arrow_left_click";
            else if (sceneGroup == 0x0100 && sceneId == 13) action = "arrow_right_click";
        } else if (command == 0x08) {
            if (arguments[0] == 0x01) {
                action = "arrow_left_hold";
                remoteSceneHolds_[sender] = "arrow_left_release";
            } else if (arguments[0] == 0x00) {
                action = "arrow_right_hold";
                remoteSceneHolds_[sender] = "arrow_right_release";
            }
        } else if (command == 0x09) {
            std::map<unsigned short, std::string>::iterator hold = remoteSceneHolds_.find(sender);
            if (hold != remoteSceneHolds_.end()) {
                releaseAction = hold->second;
                remoteSceneHolds_.erase(hold);
                action = releaseAction.c_str();
            }
        }
    }
    if (action == 0) return false;
    std::ostringstream event;
    event << "Remote key event: " << deviceLogLabel(sender) << " endpoint=" << static_cast<unsigned int>(endpoint)
          << " action=" << action << " cluster=0x" << std::hex << cluster << " command=0x"
          << static_cast<unsigned int>(command);
    if (!arguments.empty()) event << " value=" << std::dec << static_cast<unsigned int>(arguments[0]);
    if (cluster == 0x0005 && command == 0x07)
        event << " group=0x" << std::hex << sceneGroup << " scene=" << std::dec << static_cast<unsigned int>(sceneId);
    addLog(event.str());
    std::string bindingAction = action;
    if (bindingAction == "arrow_left_click") bindingAction = "arrow_left";
    else if (bindingAction == "arrow_right_click") bindingAction = "arrow_right";
    dispatchRemoteSceneAction(device->second.ieeeAddress, bindingAction);
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
    if (changed) { markViewDirty(); updateContactAlert(ieeeAddress, wasOpen); updateTemperatureAlert(device, hadTemperature, previousTemperature); saveState(); }
    return changed;
}

bool Gateway::applyIasZoneStatus(const std::string& ieeeAddress, const std::vector<unsigned char>& payload) {
    Device& device = devices_[ieeeAddress];
    bool changed;
    bool wasOpen = device.contactOpen;
    if (device.ieeeAddress.empty()) device.ieeeAddress = ieeeAddress;
    changed = ::applyIasZoneStatus(&device, payload, time(0));
    if (changed) {
        addLog(std::string("Contact status from ") + ieeeAddress + (device.contactOpen ? ": open" : ": closed"));
        updateContactAlert(ieeeAddress, wasOpen);
        saveState();
    }
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
    if (!notificationWorker_->enqueue(title, body, error)) {
        addLog(std::string("Notification queue failed: ") + title + " - " + body);
        return false;
    }
    /* The bounded coordinator log is the notification audit trail and lives
       in /tmp.  Do not append an outbox beneath runtimeDirectory_: contact
       alerts can be frequent and that directory is normally flash-backed. */
    addLog(std::string("Notification queued: ") + title + " - " + body);
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

GatewayView::GatewayView() : online(false), networkForming(false), networkFormed(false), serialRxBytes(0), ashRxFrames(0),
    joiningUntil(0), joiningPreparing(false), notificationsEnabled(false) {
}

std::string GatewayView::statusJson() const {
    std::ostringstream json;
    json << "{\"online\":" << (online ? "true" : "false") << ",\"network_forming\":"
         << (networkForming ? "true" : "false") << ",\"network_formed\":" << (networkFormed ? "true" : "false")
         << ",\"serial_rx_bytes\":" << serialRxBytes << ",\"ash_rx_frames\":" << ashRxFrames
         << ",\"joining_until\":" << joiningUntil << ",\"joining_preparing\":"
         << (joiningPreparing ? "true" : "false")
         << ",\"notifications_enabled\":" << (notificationsEnabled ? "true" : "false")
         << ",\"devices\":[";
    bool first = true;
    for (unsigned long deviceIndex = 0; deviceIndex < devices.size(); ++deviceIndex) {
        const Device& device = devices[deviceIndex];
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
             << ",\"has_light\":" << (device.hasLight ? "true" : "false")
             << ",\"light_on\":" << (device.lightOn ? "true" : "false")
             << ",\"has_brightness\":" << (device.hasBrightness ? "true" : "false")
             << ",\"brightness_percent\":" << device.brightnessPercent
             << ",\"has_remote\":" << (device.hasRemote ? "true" : "false")
             << ",\"has_low_temperature_alert\":" << (device.hasLowTemperatureAlert ? "true" : "false")
             << ",\"low_temperature_alert\":" << device.lowTemperatureAlert
             << ",\"has_high_temperature_alert\":" << (device.hasHighTemperatureAlert ? "true" : "false")
             << ",\"high_temperature_alert\":" << device.highTemperatureAlert
             << ",\"contact_initial_alert_minutes\":" << device.contactInitialAlertMinutes
             << ",\"contact_reminder_alert_minutes\":" << device.contactReminderAlertMinutes
             << ",\"sensor_refresh_interval_minutes\":" << device.sensorRefreshIntervalMinutes
             << ",\"last_seen\":" << device.lastSeen
             << ",\"manufacturer_name\":\"" << jsonEscape(device.manufacturerName)
             << "\",\"model_id\":\"" << jsonEscape(device.modelId)
             << "\",\"endpoint_descriptors\":[";
        for (unsigned long endpointIndex = 0; endpointIndex < device.endpoints.size(); ++endpointIndex) {
            const EndpointDescriptor& endpoint = device.endpoints[endpointIndex];
            if (endpointIndex != 0) json << ',';
            json << "{\"endpoint\":" << static_cast<unsigned int>(endpoint.endpoint)
                 << ",\"profile_id\":" << endpoint.profileId
                 << ",\"device_id\":" << endpoint.deviceId
                 << ",\"device_version\":" << static_cast<unsigned int>(endpoint.deviceVersion)
                 << ",\"input_clusters\":[";
            for (unsigned long clusterIndex = 0; clusterIndex < endpoint.inputClusters.size(); ++clusterIndex) {
                if (clusterIndex != 0) json << ',';
                json << endpoint.inputClusters[clusterIndex];
            }
            json << "],\"output_clusters\":[";
            for (unsigned long clusterIndex = 0; clusterIndex < endpoint.outputClusters.size(); ++clusterIndex) {
                if (clusterIndex != 0) json << ',';
                json << endpoint.outputClusters[clusterIndex];
            }
            json << "]}";
        }
        json << "]}";
    }
    json << "],\"scene_groups\":[";
    for (unsigned long groupIndex=0; groupIndex<sceneGroups.size(); ++groupIndex) {
        const SceneGroup& group=sceneGroups[groupIndex]; if (groupIndex) json << ',';
        json << "{\"id\":" << group.id << ",\"name\":\"" << jsonEscape(group.name) << "\",\"members\":[";
        for (unsigned long member=0;member<group.members.size();++member) { if(member)json<<','; json << "\"" << jsonEscape(group.members[member]) << "\""; }
        json << "],\"scenes\":[";
        for (unsigned long sceneIndex=0;sceneIndex<group.scenes.size();++sceneIndex) { const Scene& scene=group.scenes[sceneIndex]; if(sceneIndex)json<<','; json << "{\"id\":" << static_cast<unsigned int>(scene.id) << ",\"name\":\"" << jsonEscape(scene.name) << "\"}"; }
        json << "]}";
    }
    json << "],\"scene_bindings\":[";
    for (unsigned long bindingIndex=0;bindingIndex<sceneBindings.size();++bindingIndex) { const RemoteSceneBinding& binding=sceneBindings[bindingIndex]; if(bindingIndex)json<<','; json << "{\"ieee_address\":\"" << jsonEscape(binding.ieeeAddress) << "\",\"action\":\"" << jsonEscape(binding.action) << "\",\"group_id\":" << binding.groupId << ",\"off\":" << (binding.off?"true":"false") << "}"; }
    json << "]}";
    return json.str();
}

std::string GatewayView::logsJson() const {
    std::ostringstream json;
    unsigned long i;
    json << "{\"lines\":[";
    for (i = 0; i < logs.size(); ++i) {
        if (i != 0) json << ',';
        json << "\"" << jsonEscape(logs[i]) << "\"";
    }
    json << "]}";
    return json.str();
}

GatewayView Gateway::captureView() const {
    GatewayView view;
    view.online = online_; view.networkForming = networkForming_; view.networkFormed = networkFormed_;
    view.serialRxBytes = serialRxBytes_; view.ashRxFrames = ashRxFrames_; view.joiningUntil = joinUntil_;
    view.joiningPreparing = joinPreparationStep_ != 0; view.notificationsEnabled = notificationsEnabled();
    view.devices = devices(); view.sceneGroups = sceneGroups_; view.sceneBindings = sceneBindings_; view.logs = logs_;
    return view;
}

bool Gateway::takeViewDirty() { bool dirty = viewDirty_; viewDirty_ = false; return dirty; }
void Gateway::markViewDirty() { viewDirty_ = true; }
std::string Gateway::statusJson() const { return captureView().statusJson(); }
std::string Gateway::logsJson() const { return captureView().logsJson(); }

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
    std::string rendered = message.substr(0, 768);
    line << time(0) << " " << rendered;
    logs_.push_back(line.str());
    while (logs_.size() > 512) logs_.erase(logs_.begin());
    markViewDirty();
    saveLogs();
}

std::string Gateway::deviceLogLabel(unsigned short node) const {
    std::ostringstream label;
    std::map<unsigned short, std::string>::const_iterator found = nodeAddresses_.find(node);
    label << std::hex << std::setfill('0');
    if (found == nodeAddresses_.end()) {
        label << "unknown node=0x" << std::setw(4) << node;
        return label.str();
    }
    std::map<std::string, Device>::const_iterator device = devices_.find(found->second);
    if (device != devices_.end() && !device->second.name.empty()) label << device->second.name << " ";
    label << "ieee=" << found->second << " node=0x" << std::setw(4) << node;
    return label.str();
}

void Gateway::addFrameLog(const std::string& direction, unsigned short node, unsigned short profile, unsigned short cluster,
                          unsigned char sourceEndpoint, unsigned char destinationEndpoint, unsigned short options,
                          unsigned short bindingAddress, unsigned char apsSequence, unsigned char messageTag,
                          int lqi, int rssi, const std::vector<unsigned char>& message) {
    std::ostringstream line;
    line << "FRAME " << direction << " " << (direction == "TX" ? "recipient=" : "sender=") << deviceLogLabel(node)
         << " profile=0x" << std::hex << profile << " cluster=0x" << cluster;
    if (clusterName(cluster)[0] != '\0') line << "(" << clusterName(cluster) << ")";
    line << " src_ep=" << std::dec << static_cast<unsigned int>(sourceEndpoint)
         << " dst_ep=" << static_cast<unsigned int>(destinationEndpoint)
         << " aps_seq=" << static_cast<unsigned int>(apsSequence) << " options=0x" << std::hex << options
         << " binding=0x" << bindingAddress;
    if (messageTag != 0) line << " tag=" << std::dec << static_cast<unsigned int>(messageTag);
    if (lqi >= 0) line << " lqi=" << std::dec << lqi;
    if (rssi >= -128) line << " rssi=" << std::dec << rssi;
    line << " length=" << std::dec << message.size() << decodeFramePayload(profile, cluster, message)
         << " payload=" << hexBytes(message);
    addLog(line.str());
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
    std::vector<Device> volatileValues;
    NetworkState network;
    std::string error;
    unsigned long i;
    if (loadDevices(runtimeDirectory_ + "/devices.json", &saved, &error)) {
        for (i = 0; i < saved.size(); ++i) {
            clearVolatileDeviceValues(&saved[i]);
            classifyDevice(&saved[i]);
            devices_[saved[i].ieeeAddress] = saved[i];
            if (saved[i].networkAddress != 0xffff) nodeAddresses_[saved[i].networkAddress] = saved[i].ieeeAddress;
            if (saved[i].hasLight && saved[i].networkAddress != 0xffff) lightSetupPending_[saved[i].networkAddress] = true;
        }
    }
    error.clear();
    if (loadDevices("/tmp/microhad-zigbee-native-values.json", &volatileValues, &error)) {
        for (i = 0; i < volatileValues.size(); ++i) {
            if (devices_.find(volatileValues[i].ieeeAddress) != devices_.end()) {
                Device& persistent = devices_[volatileValues[i].ieeeAddress];
                if (volatileValues[i].manufacturerName.empty()) volatileValues[i].manufacturerName = persistent.manufacturerName;
                if (volatileValues[i].modelId.empty()) volatileValues[i].modelId = persistent.modelId;
                if (volatileValues[i].endpoints.empty()) volatileValues[i].endpoints = persistent.endpoints;
                classifyDevice(&volatileValues[i]);
                devices_[volatileValues[i].ieeeAddress] = volatileValues[i];
            }
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
    loadScenes();
    loadSceneRuntime();
}

void Gateway::saveState() const {
    std::vector<Device> saved = devices();
    std::string ignored;
    saveDevices("/tmp/microhad-zigbee-native-values.json", saved, &ignored);
}

void Gateway::savePersistentState() const {
    std::vector<Device> saved = devices();
    std::string ignored;
    unsigned long i;
    for (i = 0; i < saved.size(); ++i) clearVolatileDeviceValues(&saved[i]);
    saveDevices(runtimeDirectory_ + "/devices.json", saved, &ignored);
}

void Gateway::loadScenes() {
    std::string error;
    if (!loadSceneConfiguration(runtimeDirectory_ + "/scenes.conf", &sceneGroups_, &sceneBindings_, &error) && !error.empty())
        addLog(std::string("Scene configuration ignored: ") + error);
}

void Gateway::saveScenes() const {
    std::string error;
    if (!saveSceneConfiguration(runtimeDirectory_ + "/scenes.conf", sceneGroups_, sceneBindings_, &error))
        const_cast<Gateway*>(this)->addLog(std::string("Scene configuration save failed: ") + error);
}

void Gateway::loadSceneRuntime() {
    std::ifstream input("/tmp/microhad-zigbee-scenes-runtime"); std::string line;
    while (std::getline(input,line)) {
        std::istringstream fields(line); char type; fields >> type;
        if (type=='S') { unsigned int group, id; if (fields >> group >> id && group>0 && group<=0xffff && id>0 && id<256) selectedScenes_[static_cast<unsigned short>(group)]=static_cast<unsigned char>(id); }
        else if (type=='F') { unsigned int group, off; if (fields >> group >> off && group>0 && group<=0xffff) sceneGroupOff_[static_cast<unsigned short>(group)]=off!=0; }
    }
}

void Gateway::saveSceneRuntime() const {
    std::ofstream output("/tmp/microhad-zigbee-scenes-runtime", std::ios::trunc);
    if (!output) return;
    for (std::map<unsigned short,unsigned char>::const_iterator it=selectedScenes_.begin();it!=selectedScenes_.end();++it)
        output << "S " << it->first << " " << static_cast<unsigned int>(it->second) << "\n";
    for (std::map<unsigned short,bool>::const_iterator it=sceneGroupOff_.begin();it!=sceneGroupOff_.end();++it)
        output << "F " << it->first << " " << (it->second?1:0) << "\n";
}

bool Gateway::writeBytes(const std::vector<unsigned char>& bytes) {
    if (!bytes.empty()) addHexLog("SERIAL TX", &bytes[0], bytes.size());
    serialPendingBytes_.insert(serialPendingBytes_.end(), bytes.begin(), bytes.end());
    flushSerialOutput();
    return serialPendingBytes_.empty();
}

void Gateway::flushSerialOutput() {
    while (serialFd_ >= 0 && !serialPendingBytes_.empty()) {
        ssize_t count = write(serialFd_, &serialPendingBytes_[0], serialPendingBytes_.size());
        if (count > 0) serialPendingBytes_.erase(serialPendingBytes_.begin(), serialPendingBytes_.begin() + count);
        else if (count < 0 && errno == EINTR) continue;
        else break; /* Preserve the unwritten suffix, including on EAGAIN. */
    }
}

void Gateway::retransmitAsh() {
    if (ashPendingData_.empty()) return;
    if (++ashRetries_ > 3) {
        addLog("ASH acknowledgement failed repeatedly; resetting serial link");
        sendAshReset();
        return;
    }
    addLog("ASH retransmitting unacknowledged frame");
    writeBytes(AshTransport::encodeData(static_cast<unsigned char>(
        (ashSendSequence_ << 4) | 0x08 | ashReceiveSequence_), ashPendingData_));
    ashAckDeadline_ = time(0) + 2;
}

void Gateway::pollAshTransmit() {
    flushSerialOutput();
    if (!serialPendingBytes_.empty()) return;
    if (!ashPendingData_.empty()) {
        if (time(0) >= ashAckDeadline_) retransmitAsh();
        return;
    }
    if (ashTransmitQueue_.empty()) return;
    ashPendingData_ = ashTransmitQueue_.front();
    ashTransmitQueue_.erase(ashTransmitQueue_.begin());
    ashRetries_ = 0;
    ashAckDeadline_ = time(0) + 2;
    writeBytes(AshTransport::encodeData(static_cast<unsigned char>(
        (ashSendSequence_ << 4) | ashReceiveSequence_), ashPendingData_));
}

void Gateway::sendAshReset() {
    std::vector<unsigned char> raw;
    std::vector<unsigned char> reset = AshTransport::encodeData(0xc0, raw);
    reset.insert(reset.begin(), 0x1a);
    ash_.reset();
    ashTransmitQueue_.clear();
    ashPendingData_.clear();
    serialPendingBytes_.clear();
    ashAckDeadline_ = 0;
    online_ = false;
    /* Persistent state describes the intended network.  Do not send traffic
       until this NCP boot reports NETWORK_UP. */
    networkFormed_ = false;
    coordinatorIeeeAddress_.clear();
    coordinatorIeeeAddressRequested_ = false;
    interviews_.clear();
    interviewRequestInFlight_ = false;
    nextHeartbeatAt_ = 0;
    writeBytes(reset);
    resetDeadline_ = time(0) + 5;
}

unsigned char Gateway::sendEzspCommand(unsigned short command, const std::vector<unsigned char>& payload) {
    std::vector<unsigned char> ezsp;
    unsigned char sequence = ezspSequence_++;
    ezsp.push_back(sequence);
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
    {
        std::ostringstream message;
        message << "EZSP -> 0x" << std::hex << command << " payload=" << std::dec << payload.size();
        addLog(message.str());
    }
    ashTransmitQueue_.push_back(randomize(ezsp));
    pollAshTransmit();
    return sequence;
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

unsigned char Gateway::sendApsUnicast(unsigned short destination, unsigned short profile, unsigned short cluster,
                             unsigned char sourceEndpoint, unsigned char destinationEndpoint,
                             const std::vector<unsigned char>& message, unsigned char messageTag) {
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
    payload.push_back(messageTag);
    payload.push_back(static_cast<unsigned char>(message.size()));
    payload.insert(payload.end(), message.begin(), message.end());
    addFrameLog("TX", destination, profile, cluster, sourceEndpoint, destinationEndpoint, 0x0140, 0,
                static_cast<unsigned char>(apsSequence - 1), messageTag, -1, -129, message);
    return sendEzspCommand(0x0034, payload); /* sendUnicast */
}

unsigned char Gateway::sendApsMulticast(unsigned short groupId, unsigned short profile, unsigned short cluster,
                                        unsigned char sourceEndpoint, const std::vector<unsigned char>& message) {
    std::vector<unsigned char> payload;
    static unsigned char apsSequence = 0;
    payload.push_back(static_cast<unsigned char>(profile)); payload.push_back(static_cast<unsigned char>(profile >> 8));
    payload.push_back(static_cast<unsigned char>(cluster)); payload.push_back(static_cast<unsigned char>(cluster >> 8));
    payload.push_back(sourceEndpoint); payload.push_back(0xff); /* group destination has no endpoint */
    payload.push_back(0x00); payload.push_back(0x00); /* no APS retry for multicast */
    payload.push_back(static_cast<unsigned char>(groupId)); payload.push_back(static_cast<unsigned char>(groupId >> 8));
    payload.push_back(apsSequence++);
    payload.push_back(0); /* radius: stack default */
    payload.push_back(7); /* non-member radius */
    payload.push_back(0); /* message tag */
    payload.push_back(static_cast<unsigned char>(message.size()));
    payload.insert(payload.end(), message.begin(), message.end());
    addFrameLog("TX", groupId, profile, cluster, sourceEndpoint, 0xff, 0, groupId,
                static_cast<unsigned char>(apsSequence - 1), 0, -1, -129, message);
    return sendEzspCommand(0x0038, payload); /* sendMulticast */
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
    joinPreparationDeadline_ = 0;
    std::ostringstream message;
    message << "Pairing enabled for " << seconds << " seconds";
    addLog(message.str());
}

void Gateway::handleAshFrame(const std::vector<unsigned char>& frame) {
    unsigned char control;
    if (frame.empty()) return;
    ++ashRxFrames_;
    control = frame[0];
    if ((control & 0x80) == 0 || (control & 0xe0) == 0x80 || (control & 0xe0) == 0xa0) {
        if (!ashPendingData_.empty() && (control & 7) == ((ashSendSequence_ + 1) & 7)) {
            ashPendingData_.clear();
            ashSendSequence_ = static_cast<unsigned char>((ashSendSequence_ + 1) & 7);
            ashAckDeadline_ = 0;
        }
        if ((control & 0xe0) == 0xa0) {
            addLog("ASH NAK received");
            retransmitAsh();
            return;
        }
    }
    if (control == 0xc1) {
        ashPendingData_.clear();
        ashTransmitQueue_.clear();
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
        unsigned char sequence = static_cast<unsigned char>((control >> 4) & 7);
        if (sequence != ashReceiveSequence_) {
            bool duplicate = (control & 0x08) != 0;
            writeBytes(AshTransport::encodeData(static_cast<unsigned char>(
                (duplicate ? 0x80 : 0xa0) | ashReceiveSequence_), std::vector<unsigned char>()));
            addLog(duplicate ? "ASH duplicate suppressed" : "ASH receive gap; requested retransmission");
            return;
        }
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
    } else {
        command = frame[2];
        payloadOffset = 3;
        /* EZSP v5-v7 uses the legacy extended-command sentinel (0xff), then
           a reserved byte and the real 8-bit command identifier. */
        if (command == 0xff && frame.size() >= 5) {
            command = frame[4];
            payloadOffset = 5;
        }
    }
    if (command == 0) {
        if (frame.size() < payloadOffset + 4) { addLog("Truncated EZSP version reply"); return; }
        unsigned char reportedVersion = frame[payloadOffset];
        if (reportedVersion != 8 && reportedVersion != 13) {
            online_ = false; networkFormed_ = false; resetDeadline_ = 0;
            ezspVersionNegotiated_ = false;
            addLog("Unsupported EZSP protocol; only v8 and v13 are supported");
            return;
        }
        if (reportedVersion != ezspVersion_) {
            std::vector<unsigned char> version;
            ezspVersion_ = reportedVersion;
            version.push_back(ezspVersion_);
            sendEzspCommand(0x0000, version);
            return;
        }
        ezspVersionNegotiated_ = true;
    }
    {
        std::ostringstream message;
        unsigned int status = frame.size() > payloadOffset ? frame[payloadOffset] : 0xff;
        message << "EZSP <- v" << std::dec << static_cast<unsigned int>(ezspVersion_) << " 0x" << std::hex
                << command << " status=0x" << status << " bytes=" << std::dec << frame.size();
        addLog(message.str());
    }
    if (command == 0 && ezspVersionNegotiated_) {
        std::ostringstream version;
        version << "Negotiated EZSP v" << static_cast<unsigned int>(ezspVersion_)
                << " stack type=" << static_cast<unsigned int>(frame[payloadOffset + 1])
                << " stack version=0x" << std::hex
                << (static_cast<unsigned int>(frame[payloadOffset + 2]) |
                    (static_cast<unsigned int>(frame[payloadOffset + 3]) << 8));
        addLog(version.str());
        online_ = true;
        nextHeartbeatAt_ = time(0) + 20;
        if (!networkInitRequested_) {
            addLog("Configuring NCP coordinator to match original controller");
            continueCoordinatorInitialization();
        }
    }
    if (coordinatorInitializationStep_ != 0 && command == coordinatorInitializationCommand()) {
        bool truncated = frame.size() < payloadOffset + (command == 0x005a ? 4 : 1);
        /* Some v13 NCP builds fix packet-buffer allocation at compile time.
           ERROR_INVALID_ID for this optional legacy tuning is not a network
           failure. Preserve v8's allocation request and reject other errors. */
        bool fixedPacketBuffers = !truncated && ezspVersion_ == 13 &&
            coordinatorInitializationStep_ == 11 && command == 0x0053 &&
            frame[payloadOffset] == 0x37;
        if (fixedPacketBuffers) addLog("NCP uses fixed packet-buffer allocation; keeping firmware default");
        if (truncated || (!fixedPacketBuffers && command != 0x005a && frame[payloadOffset] != 0)) {
            std::ostringstream failure;
            failure << "Coordinator initialization command 0x" << std::hex << command
                    << " rejected or truncated: 0x" << (truncated ? 0xff : static_cast<unsigned int>(frame[payloadOffset]));
            addLog(failure.str());
            online_ = false;
            networkFormed_ = false;
            resetDeadline_ = 0;
            return; /* Do not proceed to network initialization after rejection. */
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
            requestCoordinatorIeeeAddress();
        } else if (frame[payloadOffset] == 0x9c || frame[payloadOffset] == 0x9d) {
            /* NETWORK_OPENED and NETWORK_CLOSED describe the permit-join
               window, not whether the coordinator left the network. */
            addLog(frame[payloadOffset] == 0x9c ?
                   "NCP Zigbee network opened for joining" :
                   "NCP Zigbee network closed for joining");
        } else {
            networkFormed_ = false;
            std::ostringstream message;
            message << "NCP Zigbee network status: 0x" << std::hex
                    << static_cast<unsigned int>(frame[payloadOffset]);
            addLog(message.str());
        }
        return;
    }
    if (command == 0x0026 && frame.size() >= payloadOffset + 8) {
        int i;
        std::ostringstream message;
        coordinatorIeeeAddress_.assign(frame.begin() + payloadOffset, frame.begin() + payloadOffset + 8);
        message << "Coordinator IEEE address: 0x" << std::hex << std::setfill('0');
        for (i = 7; i >= 0; --i) message << std::setw(2) << static_cast<unsigned int>(coordinatorIeeeAddress_[i]);
        addLog(message.str());
        return;
    }
    if (command == 0x0034 && frame[payloadOffset] == 0x91) {
        /* EMBER_NETWORK_DOWN: stop advertising the saved state as live and
           hold subsequent refreshes until the NCP reports NETWORK_UP. */
        networkFormed_ = false;
        interviews_.clear();
        interviewRequestInFlight_ = false;
        addLog("NCP rejected Zigbee transmit: network is down");
        return;
    }
    if (command == 0x0038 || command == 0x0034) {
        for (std::map<unsigned short, SceneRetry>::iterator it=sceneRetries_.begin(); it!=sceneRetries_.end(); ++it) {
            SceneRetry& retry=it->second;
            if (!retry.inFlight || retry.ezspSequence != frame[0] ||
                (retry.unicast ? command != 0x0034 : command != 0x0038)) continue;
            unsigned char status=frame[payloadOffset];
            bool stale=retry.sentOff!=retry.off || (!retry.off && retry.sentSceneId!=retry.sceneId);
            retry.inFlight=false;
            if (stale) {
                retry.unicast=false; retry.memberIndex=0;
                retry.retryRequested=false;
                retry.retryAt=time(0);
            } else if (status == 0 && retry.unicast) {
                ++retry.memberIndex; retry.attempts=0;
                retry.retryRequested=false; retry.retryAt=time(0);
            } else if (status == 0xa1 && !retry.unicast && findSceneGroup(it->first) != 0) {
                retry.unicast=true; retry.memberIndex=0; retry.attempts=0;
                retry.retryRequested=false; retry.retryAt=time(0);
                addLog("Scene multicast busy (0xa1); falling back to serialized unicast");
            } else if (status==0) {
                completeSceneRetry(it->first,&retry);
                retry.retryRequested=false;
                retry.retryAt=LONG_MAX;
            } else {
                long now=time(0);
                bool immediate=retry.retryRequested;
                bool scheduled;
                retry.retryRequested=false;
                if (immediate) {
                    retry.retryAt=now;
                    scheduled=true;
                } else scheduled=scheduleSceneRetry(&retry, now);
                std::ostringstream message;
                message << (retry.unicast ? "Scene unicast rejected: 0x" : "Scene multicast rejected: 0x") << std::hex << static_cast<unsigned int>(status);
                if (immediate) message << "; retry requested, retrying immediately";
                else if (scheduled) message << "; retrying in " << std::dec << retry.retryAt-now << "s";
                else message << "; giving up";
                addLog(message.str());
            }
            return;
        }
    }
    if (command == 0x0034 && interviewRequestInFlight_ && !interviewSendAccepted_ && frame[0] == interviewEzspSequence_) {
        if (frame[payloadOffset] == 0) interviewSendAccepted_ = true;
        else {
            std::ostringstream reason;
            reason << "NCP rejected transmit 0x" << std::hex << static_cast<unsigned int>(frame[payloadOffset]);
            failInterviewOperation(interviewNode_, reason.str(), true);
        }
        return;
    }
    if (command == 0x003f && interviewRequestInFlight_ && frame.size() >= payloadOffset + 16) {
        unsigned char messageTag = frame[payloadOffset + 14];
        unsigned char deliveryStatus = frame[payloadOffset + 15];
        std::map<unsigned short, InterviewState>::iterator interview = interviews_.find(interviewNode_);
        if (interview != interviews_.end() && interview->second.operationIndex < interview->second.operations.size() &&
            interview->second.operations[interview->second.operationIndex].messageTag == messageTag && deliveryStatus != 0) {
            std::ostringstream reason;
            reason << "delivery failed 0x" << std::hex << static_cast<unsigned int>(deliveryStatus);
            failInterviewOperation(interviewNode_, reason.str());
        }
        return;
    }
    if (joinPreparationStep_ == 1 && command == 0x0055) {
        if (frame[payloadOffset] == 0) {
            std::vector<unsigned char> policy;
            policy.push_back(5); /* TC_KEY_REQUEST_POLICY */
            policy.push_back(0x51); policy.push_back(0x00); /* permit global trust-center key requests */
            joinPreparationStep_ = 2;
            joinPreparationDeadline_ = time(0) + 10;
            sendEzspCommand(0x0055, policy);
        } else {
            fprintf(stderr, "cannot set Zigbee application-key policy: 0x%02x\n", frame[payloadOffset]);
            addLog("Cannot set Zigbee application-key policy");
            joinPreparationStep_ = 0;
            joinPreparationDeadline_ = 0;
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
            joinPreparationDeadline_ = time(0) + 10;
            addLog("Preparing secure pairing window");
            if (ezspVersion_ == 13) {
                payload.push_back(0); /* SecurityManagerContextFlags.NONE */
                sendEzspCommand(0x0111, payload); /* importTransientKey */
            } else sendEzspCommand(0x00af, payload); /* v8 addTransientLinkKey */
        }
        else {
            fprintf(stderr, "cannot set Zigbee trust-center key policy: 0x%02x\n", frame[payloadOffset]);
            addLog("Cannot set Zigbee trust-center key policy");
            joinPreparationStep_ = 0;
            joinPreparationDeadline_ = 0;
            pendingJoinSeconds_ = 0;
        }
        return;
    }
    if (joinPreparationStep_ == 3 && command == (ezspVersion_ == 13 ? 0x0111 : 0x00af)) {
        unsigned long width = ezspVersion_ == 13 ? 4 : 1;
        bool success = frame.size() >= payloadOffset + width;
        for (unsigned long i = 0; success && i < width; ++i)
            if (frame[payloadOffset + i] != 0) success = false;
        if (success) {
            std::vector<unsigned char> policy;
            policy.push_back(0); /* TRUST_CENTER_POLICY */
            policy.push_back(3); policy.push_back(0); /* allow joins and unsecured rejoins */
            joinPreparationStep_ = 4;
            joinPreparationDeadline_ = time(0) + 10;
            sendEzspCommand(0x0055, policy);
        } else {
            addLog("Cannot add Zigbee transient joining key: rejected or truncated status");
            joinPreparationStep_ = 0;
            joinPreparationDeadline_ = 0;
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
            joinPreparationDeadline_ = 0;
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
        requestCoordinatorIeeeAddress();
        if (!stackTokenWritingRequested_) enableStackTokenWriting();
    }
}

void Gateway::registerNode(unsigned short nodeId, const unsigned char* eui64) {
    std::ostringstream address;
    int i;
    address << "0x" << std::hex << std::setfill('0');
    /* EZSP transmits EUI64 least significant byte first. */
    for (i = 7; i >= 0; --i) address << std::setw(2) << static_cast<unsigned int>(eui64[i]);
    if (nodeAddresses_.count(nodeId) && nodeAddresses_[nodeId] != address.str()) {
        devices_[nodeAddresses_[nodeId]].networkAddress = 0xffff;
        interviews_.erase(nodeId);
        lastActiveEndpointRequestAt_.erase(nodeId);
        if (interviewRequestInFlight_ && interviewNode_ == nodeId) {
            interviewRequestInFlight_ = false;
            interviewDeadline_ = 0;
        }
    }
    /* A rejoin may allocate a different short address.  Move an unfinished
       interview rather than leaving response matching tied to the stale
       address. */
    std::map<unsigned short, std::string>::iterator oldNode = nodeAddresses_.begin();
    while (oldNode != nodeAddresses_.end()) {
        std::map<unsigned short, std::string>::iterator current = oldNode++;
        if (current->first == nodeId || current->second != address.str()) continue;
        std::map<unsigned short, InterviewState>::iterator oldInterview = interviews_.find(current->first);
        if (oldInterview != interviews_.end()) {
            InterviewState moved = oldInterview->second;
            moved.paused = false;
            if (moved.operationIndex < moved.operations.size()) moved.operations[moved.operationIndex].attempts = 0;
            interviews_.erase(oldInterview);
            if (interviewRequestInFlight_ && interviewNode_ == nodeId) {
                interviewRequestInFlight_ = false;
                interviewDeadline_ = 0;
            }
            if (moved.operationIndex < moved.operations.size()) {
                moved.operations[moved.operationIndex].transactionSequenceAssigned = false;
                moved.operations[moved.operationIndex].retryAt = 0;
            }
            interviews_[nodeId] = moved;
        }
        if (interviewRequestInFlight_ && interviewNode_ == current->first) {
            interviewRequestInFlight_ = false;
            interviewDeadline_ = 0;
        }
        nextSensorRefreshAt_.erase(current->first);
        lastActiveEndpointRequestAt_.erase(current->first);
        nodeAddresses_.erase(current);
        addLog("Migrated device state from stale network address");
    }
    nodeAddresses_[nodeId] = address.str();
    Device& device = devices_[address.str()];
    bool persistentChanged = device.networkAddress != nodeId;
    device.networkAddress = nodeId;
    if (device.sensorRefreshIntervalMinutes == 0) nextSensorRefreshAt_.erase(nodeId);
    else nextSensorRefreshAt_[nodeId] = sensorRefreshDeadline(time(0), device.sensorRefreshIntervalMinutes);
    if (device.ieeeAddress.empty()) {
        device.ieeeAddress = address.str();
        device.name = address.str();
        device.lastSeen = time(0);
        persistentChanged = true;
    }
    saveState();
    if (persistentChanged) savePersistentState();
    std::ostringstream message;
    message << "Device announced: " << address.str() << " (0x" << std::hex << nodeId << ")";
    addLog(message.str());
    wakeInterview(nodeId);
    /* EZSP reports the same rejoin through more than one callback.  One
       endpoint discovery/interview is sufficient and prevents overlapping
       sleepy-device setup transactions. */
    if (lastActiveEndpointRequestAt_[nodeId] == 0 || time(0) - lastActiveEndpointRequestAt_[nodeId] >= 30) {
        lastActiveEndpointRequestAt_[nodeId] = time(0);
        startInterview(nodeId, false);
    } else addLog("Ignoring duplicate device announcement during interview");
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
            savePersistentState();
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

void Gateway::startInterview(unsigned short destination, bool refreshOnly) {
    std::map<unsigned short, std::string>::const_iterator address = nodeAddresses_.find(destination);
    if (address == nodeAddresses_.end()) return;
    std::map<unsigned short, InterviewState>::iterator existing = interviews_.find(destination);
    if (existing != interviews_.end()) {
        existing->second.paused = false;
        return;
    }
    InterviewState state;
    state.ieeeAddress = address->second;
    state.refreshOnly = refreshOnly;
    interviews_[destination] = state;
    queueInterviewOperation(destination, InterviewNodeDescriptor, 0);
}

void Gateway::queueInterviewOperation(unsigned short destination, InterviewOperationType type, unsigned char endpoint,
                                      unsigned short cluster, unsigned short attribute, unsigned char dataType,
                                      unsigned short reportableChange, unsigned short minimumInterval,
                                      unsigned short maximumInterval) {
    InterviewOperation operation;
    operation.type = type;
    operation.endpoint = endpoint;
    operation.cluster = cluster;
    operation.attribute = attribute;
    operation.dataType = dataType;
    operation.reportableChange = reportableChange;
    operation.minimumInterval = minimumInterval;
    operation.maximumInterval = maximumInterval;
    interviews_[destination].operations.push_back(operation);
}

bool Gateway::endpointSupports(const EndpointDescriptor& endpoint, unsigned short cluster) const {
    return std::find(endpoint.inputClusters.begin(), endpoint.inputClusters.end(), cluster) != endpoint.inputClusters.end();
}

void Gateway::buildDescriptorOperations(unsigned short destination, const std::vector<unsigned char>& endpoints) {
    InterviewState& state = interviews_[destination];
    state.operations.clear();
    state.operationIndex = 0;
    state.discoveredEndpoints.clear();
    for (unsigned long i = 0; i < endpoints.size(); ++i) {
        if (endpoints[i] != 0) queueInterviewOperation(destination, InterviewSimpleDescriptor, endpoints[i]);
    }
    if (state.operations.empty()) {
        state.paused = true;
        addLog("Interview paused: device returned no valid endpoints");
    }
}

void Gateway::buildRefreshOperations(unsigned short destination) {
    InterviewState& state = interviews_[destination];
    const Device& device = devices_[state.ieeeAddress];
    state.operations.clear();
    state.operationIndex = 0;
    for (unsigned long i = 0; i < device.endpoints.size(); ++i) {
        const EndpointDescriptor& endpoint = device.endpoints[i];
        if (endpointSupports(endpoint, 0x0402)) queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0402, 0x0000);
        if (endpointSupports(endpoint, 0x0405)) queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0405, 0x0000);
        if (endpointSupports(endpoint, 0x0006)) queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0006, 0x0000);
        if (endpointSupports(endpoint, 0x0008)) queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0008, 0x0000);
        if (endpointSupports(endpoint, 0x0001)) {
            queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0001, 0x0021);
            queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0001, 0x0020);
        }
    }
    if (state.operations.empty()) {
        addLog("Refresh completed: no persisted measurement endpoints");
        interviews_.erase(destination);
    }
}

void Gateway::buildConfigurationOperations(unsigned short destination) {
    InterviewState& state = interviews_[destination];
    Device& device = devices_[state.ieeeAddress];
    state.operations.clear();
    state.operationIndex = 0;
    for (unsigned long i = 0; i < device.endpoints.size(); ++i) {
        const EndpointDescriptor& endpoint = device.endpoints[i];
        if (endpointSupports(endpoint, 0x0402)) queueInterviewOperation(destination, InterviewBind, endpoint.endpoint, 0x0402);
        if (endpointSupports(endpoint, 0x0405)) queueInterviewOperation(destination, InterviewBind, endpoint.endpoint, 0x0405);
        if (endpointSupports(endpoint, 0x0001)) queueInterviewOperation(destination, InterviewBind, endpoint.endpoint, 0x0001);
        if (endpointSupports(endpoint, 0x0406)) queueInterviewOperation(destination, InterviewBind, endpoint.endpoint, 0x0406);
        if (endpointSupports(endpoint, 0x0006)) queueInterviewOperation(destination, InterviewBind, endpoint.endpoint, 0x0006);
        if (endpointSupports(endpoint, 0x0008)) queueInterviewOperation(destination, InterviewBind, endpoint.endpoint, 0x0008);
        /* Remotes are clients of On/Off and Level Control, so those clusters
           appear in the output list.  Binding only input clusters configures
           sensor reports but leaves remotes (including IKEA E1743) sending
           their commands to their old/default target instead of us. */
        if (std::find(endpoint.outputClusters.begin(), endpoint.outputClusters.end(), 0x0006) != endpoint.outputClusters.end())
            queueInterviewOperation(destination, InterviewBind, endpoint.endpoint, 0x0006);
        if (std::find(endpoint.outputClusters.begin(), endpoint.outputClusters.end(), 0x0008) != endpoint.outputClusters.end())
            queueInterviewOperation(destination, InterviewBind, endpoint.endpoint, 0x0008);
    }
    for (unsigned long i = 0; i < device.endpoints.size(); ++i) {
        const EndpointDescriptor& endpoint = device.endpoints[i];
        if (endpointSupports(endpoint, 0x0402)) {
            queueInterviewOperation(destination, InterviewConfigureReporting, endpoint.endpoint, 0x0402, 0x0000, 0x29, 20, 30, 3600);
            queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0402, 0x0000);
        }
        if (endpointSupports(endpoint, 0x0405)) {
            queueInterviewOperation(destination, InterviewConfigureReporting, endpoint.endpoint, 0x0405, 0x0000, 0x21, 100, 30, 3600);
            queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0405, 0x0000);
        }
        if (endpointSupports(endpoint, 0x0001)) {
            queueInterviewOperation(destination, InterviewConfigureReporting, endpoint.endpoint, 0x0001, 0x0021, 0x20, 1, 3600, 65000);
            queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0001, 0x0021);
            queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0001, 0x0020);
        }
        if (endpointSupports(endpoint, 0x0406)) {
            queueInterviewOperation(destination, InterviewConfigureReporting, endpoint.endpoint, 0x0406, 0x0000, 0x18, 0, 0, 3600);
            queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0406, 0x0000);
        }
        if (endpointSupports(endpoint, 0x0006)) {
            queueInterviewOperation(destination, InterviewConfigureReporting, endpoint.endpoint, 0x0006, 0x0000, 0x10, 1, 0, 3600);
            queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0006, 0x0000);
        }
        if (endpointSupports(endpoint, 0x0008)) {
            queueInterviewOperation(destination, InterviewConfigureReporting, endpoint.endpoint, 0x0008, 0x0000, 0x20, 1, 0, 3600);
            queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0008, 0x0000);
        }
        if (endpointSupports(endpoint, 0x0000) &&
            (device.manufacturerName.find("LUMI") != std::string::npos || device.manufacturerName.find("Xiaomi") != std::string::npos ||
             device.modelId.find("lumi.") == 0)) {
            queueInterviewOperation(destination, InterviewAttributeRead, endpoint.endpoint, 0x0000, 0xff01);
        }
    }
    if (state.operations.empty()) {
        addLog("Interview completed: no configurable sensor clusters");
        interviews_.erase(destination);
    }
}

void Gateway::sendNextInterviewOperation() {
    std::map<unsigned short, InterviewState>::iterator interview;
    for (interview = interviews_.begin(); interview != interviews_.end(); ++interview) {
        InterviewState& state = interview->second;
        if (state.paused || state.operationIndex >= state.operations.size()) continue;
        if (state.operations[state.operationIndex].retryAt > time(0)) continue;
        sendInterviewOperation(interview->first, &state.operations[state.operationIndex]);
        return;
    }
}

void Gateway::sendInterviewOperation(unsigned short destination, InterviewOperation* operation) {
    std::vector<unsigned char> request;
    /* Keep the transaction sequence stable across retries.  A sleepy child
       can answer just after the timeout; replacing the sequence made that
       valid response look unrelated to the retry. */
    if (!operation->transactionSequenceAssigned) {
        operation->transactionSequence = operation->type == InterviewNodeDescriptor || operation->type == InterviewActiveEndpoints ||
            operation->type == InterviewSimpleDescriptor || operation->type == InterviewBind ? zdoSequence_++ : zclTransactionSequence_++;
        operation->transactionSequenceAssigned = true;
    }
    operation->messageTag = nextMessageTag_++;
    if (nextMessageTag_ == 0) nextMessageTag_ = 1;
    ++operation->attempts;
    interviewRequestInFlight_ = true;
    interviewSendAccepted_ = false;
    interviewEzspSequence_ = ezspSequence_;
    interviewNode_ = destination;
    interviewDeadline_ = time(0) + 20;
    if (operation->type == InterviewNodeDescriptor) {
        request.push_back(operation->transactionSequence);
        request.push_back(static_cast<unsigned char>(destination));
        request.push_back(static_cast<unsigned char>(destination >> 8));
        sendApsUnicast(destination, 0x0000, 0x0002, 0, 0, request, operation->messageTag);
    } else if (operation->type == InterviewActiveEndpoints) {
        request.push_back(operation->transactionSequence);
        request.push_back(static_cast<unsigned char>(destination));
        request.push_back(static_cast<unsigned char>(destination >> 8));
        sendApsUnicast(destination, 0x0000, 0x0005, 0, 0, request, operation->messageTag);
    } else if (operation->type == InterviewSimpleDescriptor) {
        request.push_back(operation->transactionSequence);
        request.push_back(static_cast<unsigned char>(destination));
        request.push_back(static_cast<unsigned char>(destination >> 8));
        request.push_back(operation->endpoint);
        sendApsUnicast(destination, 0x0000, 0x0004, 0, 0, request, operation->messageTag);
    } else if (operation->type == InterviewBind) {
        sendZdoBindRequest(destination, operation->endpoint, operation->cluster, interviews_[destination].ieeeAddress,
                           operation->transactionSequence, operation->messageTag);
    } else if (operation->type == InterviewBasicRead) {
        sendZclReadBasic(destination, operation->endpoint, operation->transactionSequence, operation->messageTag);
    } else if (operation->type == InterviewAttributeRead) {
        sendZclReadAttribute(destination, operation->endpoint, operation->cluster, operation->attribute,
                             operation->transactionSequence, operation->messageTag);
    } else {
        sendZclConfigureReporting(destination, operation->endpoint, operation->cluster, operation->attribute,
                                  operation->dataType, operation->minimumInterval, operation->maximumInterval,
                                  operation->reportableChange, operation->transactionSequence, operation->messageTag);
    }
}

void Gateway::failInterviewOperation(unsigned short destination, const std::string& reason, bool localFailure) {
    std::map<unsigned short, InterviewState>::iterator found = interviews_.find(destination);
    interviewRequestInFlight_ = false;
    interviewDeadline_ = 0;
    if (found == interviews_.end() || found->second.operationIndex >= found->second.operations.size()) return;
    std::ostringstream message;
    message << "Interview operation failed for " << found->second.ieeeAddress << ": " << reason;
    addLog(message.str());
    InterviewOperation& operation = found->second.operations[found->second.operationIndex];
    if (localFailure && operation.attempts < 3) {
        operation.retryAt = time(0) + 2;
        found->second.paused = false;
        addLog("Coordinator-local failure; retry scheduled in two seconds");
        return;
    }
    /* Retrying immediately cannot wake a sleepy child and can contend with
       unrelated sensor reports.  Keep this operation pending until the
       device next talks or the five-minute refresh wakes the state machine. */
    found->second.paused = true;
    addLog("Interview paused until the device wakes again");
}

void Gateway::wakeInterview(unsigned short destination) {
    std::map<unsigned short, InterviewState>::iterator found = interviews_.find(destination);
    if (found != interviews_.end() && found->second.paused) {
        found->second.paused = false;
        if (found->second.operationIndex < found->second.operations.size())
            found->second.operations[found->second.operationIndex].attempts = 0;
        addLog("Resuming paused interview on device traffic");
    }
}

bool Gateway::parseSimpleDescriptor(const std::vector<unsigned char>& message, EndpointDescriptor* descriptor) const {
    unsigned long offset;
    unsigned long count;
    if (message.size() < 13 || message[1] != 0 || message[4] < 8 ||
        message.size() < 5UL + static_cast<unsigned long>(message[4])) return false;
    descriptor->endpoint = message[5];
    descriptor->profileId = static_cast<unsigned short>(message[6]) | (static_cast<unsigned short>(message[7]) << 8);
    descriptor->deviceId = static_cast<unsigned short>(message[8]) | (static_cast<unsigned short>(message[9]) << 8);
    descriptor->deviceVersion = message[10] & 0x0f;
    count = message[11];
    offset = 12;
    if (offset + count * 2 + 1 > message.size()) return false;
    descriptor->inputClusters.clear();
    descriptor->outputClusters.clear();
    for (unsigned long i = 0; i < count; ++i, offset += 2) {
        descriptor->inputClusters.push_back(static_cast<unsigned short>(message[offset]) |
                                            (static_cast<unsigned short>(message[offset + 1]) << 8));
    }
    count = message[offset++];
    if (offset + count * 2 > message.size()) return false;
    for (unsigned long i = 0; i < count; ++i, offset += 2) {
        descriptor->outputClusters.push_back(static_cast<unsigned short>(message[offset]) |
                                             (static_cast<unsigned short>(message[offset + 1]) << 8));
    }
    return true;
}

bool Gateway::updateBasicIdentity(Device* device, const std::vector<unsigned char>& records) {
    unsigned long offset = 0;
    bool changed = false;
    while (offset + 3 <= records.size()) {
        unsigned short attribute = static_cast<unsigned short>(records[offset]) |
                                   (static_cast<unsigned short>(records[offset + 1]) << 8);
        unsigned char status = records[offset + 2];
        if (status != 0) { offset += 3; continue; }
        if (offset + 5 > records.size()) break;
        unsigned char type = records[offset + 3];
        if (type != 0x42 || offset + 5 + records[offset + 4] > records.size()) break;
        std::string value(records.begin() + offset + 5, records.begin() + offset + 5 + records[offset + 4]);
        if (attribute == 0x0004 && device->manufacturerName != value) {
            device->manufacturerName = value;
            changed = true;
        } else if (attribute == 0x0005 && device->modelId != value) {
            device->modelId = value;
            changed = true;
        }
        offset += 5 + records[offset + 4];
    }
    return changed;
}

void Gateway::completeInterviewOperation(unsigned short destination) {
    std::map<unsigned short, InterviewState>::iterator found = interviews_.find(destination);
    if (interviewRequestInFlight_ && interviewNode_ == destination) {
        interviewRequestInFlight_ = false;
        interviewDeadline_ = 0;
    }
    if (found == interviews_.end() || found->second.operationIndex >= found->second.operations.size()) return;
    found->second.paused = false;
    InterviewOperationType completedType = found->second.operations[found->second.operationIndex].type;
    ++found->second.operationIndex;
    if (found->second.operationIndex < found->second.operations.size()) return;

    InterviewState& state = found->second;
    Device& device = devices_[state.ieeeAddress];
    if (completedType == InterviewNodeDescriptor) {
        state.operations.clear();
        state.operationIndex = 0;
        queueInterviewOperation(destination, InterviewActiveEndpoints, 0);
    } else if (completedType == InterviewSimpleDescriptor) {
        if (!endpointDescriptorsEqual(device.endpoints, state.discoveredEndpoints)) {
            device.endpoints = state.discoveredEndpoints;
            classifyDevice(&device);
            state.persistentDirty = true;
        }
        if (state.persistentDirty) {
            savePersistentState();
            state.persistentDirty = false;
        }
        state.operations.clear();
        state.operationIndex = 0;
        for (unsigned long i = 0; i < device.endpoints.size(); ++i) {
            if (endpointSupports(device.endpoints[i], 0x0000))
                queueInterviewOperation(destination, InterviewBasicRead, device.endpoints[i].endpoint, 0x0000);
        }
        if (state.operations.empty()) {
            buildConfigurationOperations(destination);
        }
    } else if (completedType == InterviewBasicRead) {
        if (state.persistentDirty) {
            savePersistentState();
            state.persistentDirty = false;
        }
        buildConfigurationOperations(destination);
    } else {
        addLog(state.refreshOnly ? "Sensor attribute refresh completed" : "Sensor interview completed");
        interviews_.erase(destination);
    }
}

void Gateway::requestCoordinatorIeeeAddress() {
    if (coordinatorIeeeAddressRequested_) return;
    coordinatorIeeeAddressRequested_ = true;
    addLog("Requesting coordinator IEEE address for device reporting bindings");
    sendEzspCommand(0x0026, std::vector<unsigned char>()); /* getEui64 */
}

void Gateway::sendZdoBindRequest(unsigned short destination, unsigned char endpoint, unsigned short cluster,
                                 const std::string& sourceIeeeAddress, unsigned char transactionSequence,
                                 unsigned char messageTag) {
    std::vector<unsigned char> request;
    int i;
    if (coordinatorIeeeAddress_.size() != 8 || sourceIeeeAddress.size() != 18 ||
        sourceIeeeAddress[0] != '0' || (sourceIeeeAddress[1] != 'x' && sourceIeeeAddress[1] != 'X')) {
        addLog("Cannot bind sensor reporting: missing IEEE address");
        return;
    }
    request.push_back(transactionSequence);
    /* The stored display form is most-significant byte first; ZDO uses little endian. */
    for (i = 7; i >= 0; --i) {
        unsigned int byte = 0;
        std::istringstream parser(sourceIeeeAddress.substr(2 + i * 2, 2));
        parser >> std::hex >> byte;
        request.push_back(static_cast<unsigned char>(byte));
    }
    request.push_back(endpoint);
    request.push_back(static_cast<unsigned char>(cluster));
    request.push_back(static_cast<unsigned char>(cluster >> 8));
    request.push_back(0x03); /* 64-bit destination address */
    request.insert(request.end(), coordinatorIeeeAddress_.begin(), coordinatorIeeeAddress_.end());
    request.push_back(1); /* coordinator endpoint */
    sendApsUnicast(destination, 0x0000, 0x0021, 0, 0, request, messageTag);
    {
        std::ostringstream message;
        message << "ZDO bind request sent: cluster 0x" << std::hex << cluster;
        addLog(message.str());
    }
}

void Gateway::sendZclReadAttribute(unsigned short destination, unsigned char endpoint, unsigned short cluster,
                                   unsigned short attribute, unsigned char transactionSequence, unsigned char messageTag) {
    std::vector<unsigned char> request;
    request.push_back(0x10); /* global command, client-to-server, disable default response */
    request.push_back(transactionSequence);
    request.push_back(0x00); /* Read Attributes */
    request.push_back(static_cast<unsigned char>(attribute));
    request.push_back(static_cast<unsigned char>(attribute >> 8));
    sendApsUnicast(destination, 0x0104, cluster, 1, endpoint, request, messageTag);
}

void Gateway::sendZclReadBasic(unsigned short destination, unsigned char endpoint,
                               unsigned char transactionSequence, unsigned char messageTag) {
    std::vector<unsigned char> request;
    request.push_back(0x10);
    request.push_back(transactionSequence);
    request.push_back(0x00);
    request.push_back(0x04); request.push_back(0x00);
    request.push_back(0x05); request.push_back(0x00);
    sendApsUnicast(destination, 0x0104, 0x0000, 1, endpoint, request, messageTag);
}

void Gateway::sendZclConfigureReporting(unsigned short destination, unsigned char endpoint, unsigned short cluster,
                                        unsigned short attribute, unsigned char dataType, unsigned short minimumInterval,
                                        unsigned short maximumInterval, unsigned short reportableChange,
                                        unsigned char transactionSequence, unsigned char messageTag) {
    std::vector<unsigned char> request = buildZclConfigureReporting(transactionSequence, attribute, dataType,
                                                                    minimumInterval, maximumInterval, reportableChange);
    sendApsUnicast(destination, 0x0104, cluster, 1, endpoint, request, messageTag);
}

void Gateway::sendZclDefaultResponse(unsigned short destination, unsigned char sourceEndpoint, unsigned char destinationEndpoint,
                                     unsigned short cluster, unsigned char transactionSequence, unsigned char command) {
    std::vector<unsigned char> response;
    response.push_back(0x18); /* global, server-to-client, disable response to this response */
    response.push_back(transactionSequence);
    response.push_back(0x0b); /* Default Response */
    response.push_back(command);
    response.push_back(0x00); /* success */
    sendApsUnicast(destination, 0x0104, cluster, sourceEndpoint, destinationEndpoint, response);
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
    /* A ZDO response consists of only its transaction sequence and status.
       In particular Bind_rsp (0x8021) is valid at two bytes. */
    if (messageOffset + length > payload.size() || length < 2) return;
    message.assign(payload.begin() + messageOffset, payload.begin() + messageOffset + length);
    addFrameLog("RX", sender,
                static_cast<unsigned short>(payload[1]) | (static_cast<unsigned short>(payload[2]) << 8), cluster,
                payload[5], payload[6], static_cast<unsigned short>(payload[7]) | (static_cast<unsigned short>(payload[8]) << 8),
                static_cast<unsigned short>(payload[16]) | (static_cast<unsigned short>(payload[17]) << 8), payload[11], 0,
                static_cast<int>(payload[12]), static_cast<int>(static_cast<signed char>(payload[13])), message);
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
    if (cluster == 0x8002 && message.size() >= 2 && message[1] == 0) {
        std::map<unsigned short, InterviewState>::iterator interview = interviews_.find(sender);
        if (interview != interviews_.end() &&
            interview->second.operationIndex < interview->second.operations.size() &&
            interview->second.operations[interview->second.operationIndex].transactionSequenceAssigned) {
            InterviewOperation& operation = interview->second.operations[interview->second.operationIndex];
            if (operation.type == InterviewNodeDescriptor && operation.transactionSequence == message[0]) {
                addLog("Node descriptor received");
                completeInterviewOperation(sender);
            }
        }
        return;
    }
    if (cluster == 0x8005 && message.size() >= 5 && message[1] == 0) {
        std::map<unsigned short, InterviewState>::iterator interview = interviews_.find(sender);
        if (interview != interviews_.end() &&
            interview->second.operationIndex < interview->second.operations.size() &&
            interview->second.operations[interview->second.operationIndex].transactionSequenceAssigned) {
            InterviewOperation& operation = interview->second.operations[interview->second.operationIndex];
            unsigned long count = message[4];
            if (operation.type != InterviewActiveEndpoints || operation.transactionSequence != message[0] ||
                message.size() < 5 + count) return;
            std::vector<unsigned char> endpoints(message.begin() + 5, message.begin() + 5 + count);
            if (interviewRequestInFlight_ && interviewNode_ == sender) {
                interviewRequestInFlight_ = false;
                interviewDeadline_ = 0;
            }
            interview->second.paused = false;
            if (interview->second.refreshOnly && !devices_[interview->second.ieeeAddress].endpoints.empty())
                buildRefreshOperations(sender);
            else buildDescriptorOperations(sender, endpoints);
            addLog("Active endpoints received");
        }
        return;
    }
    if (cluster == 0x8004 && message.size() >= 5) {
        std::map<unsigned short, InterviewState>::iterator interview = interviews_.find(sender);
        if (interview != interviews_.end() &&
            interview->second.operationIndex < interview->second.operations.size() &&
            interview->second.operations[interview->second.operationIndex].transactionSequenceAssigned) {
            InterviewOperation& operation = interview->second.operations[interview->second.operationIndex];
            EndpointDescriptor descriptor;
            if (operation.type == InterviewSimpleDescriptor && operation.transactionSequence == message[0] &&
                parseSimpleDescriptor(message, &descriptor) && descriptor.endpoint == operation.endpoint) {
                interview->second.discoveredEndpoints.push_back(descriptor);
                addLog("Simple descriptor received");
                completeInterviewOperation(sender);
            }
        }
        return;
    }
    if (cluster == 0x8021 && message.size() >= 2) {
        std::ostringstream bind;
        bind << "ZDO bind response: " << (message[1] == 0 ? "accepted" : "rejected 0x") << std::hex;
        if (message[1] != 0) bind << static_cast<unsigned int>(message[1]);
        addLog(bind.str());
        std::map<unsigned short, InterviewState>::iterator interview = interviews_.find(sender);
        if (interview != interviews_.end() &&
            interview->second.operationIndex < interview->second.operations.size() &&
            interview->second.operations[interview->second.operationIndex].transactionSequenceAssigned) {
            InterviewOperation& operation = interview->second.operations[interview->second.operationIndex];
            if (operation.type == InterviewBind && operation.transactionSequence == message[0]) completeInterviewOperation(sender);
        }
        return;
    }
    wakeInterview(sender);
    address = nodeAddresses_.find(sender);
    if (address == nodeAddresses_.end()) return;
    unsigned char frameControl = message[0];
    unsigned long zclOffset = 3;
    unsigned short manufacturer = 0;
    if ((frameControl & 0x04) != 0) zclOffset += 2;
    if (zclOffset > message.size()) return;
    unsigned char command = message[zclOffset - 1];
    unsigned char transactionSequence = message[zclOffset - 2];
    if ((frameControl & 0x04) != 0)
        manufacturer = static_cast<unsigned short>(message[1]) | (static_cast<unsigned short>(message[2]) << 8);
    {
        std::map<unsigned short, std::string>::iterator known = nodeAddresses_.find(sender);
        if (known != nodeAddresses_.end()) {
            Device& device = devices_[known->second];
            device.linkQuality = payload[12];
            device.lastSeen = time(0);
            markViewDirty();
        }
    }
    if ((frameControl & 0x03) == 0x01) {
        std::vector<unsigned char> arguments(message.begin() + zclOffset, message.end());
        if (logRemoteKeyEvent(sender, cluster, payload[5], manufacturer, command, arguments) && (frameControl & 0x10) == 0)
            sendZclDefaultResponse(sender, payload[6], payload[5], cluster, transactionSequence, command);
    }
    if (cluster == 0x0500 && command == 0x01 && message.size() >= zclOffset + 4) {
        int zoneId = allocateIasZoneId(address->second);
        if (zoneId >= 0) {
            sendIasEnrollResponse(sender, payload[6], payload[5], message[zclOffset - 2],
                                  static_cast<unsigned char>(zoneId));
        }
    } else if (cluster == 0x0500 && command == 0x00 && message.size() >= zclOffset + 2) {
        std::vector<unsigned char> zone(message.begin() + zclOffset, message.end());
        applyIasZoneStatus(address->second, zone);
    } else if ((frameControl & 0x03) == 0 && command == 0x01) {
        std::vector<unsigned char> records(message.begin() + zclOffset, message.end());
        bool wasOpen = devices_[address->second].contactOpen;
        std::map<unsigned short, InterviewState>::iterator interview = interviews_.find(sender);
        bool matched = false;
        if (interview != interviews_.end() &&
            interview->second.operationIndex < interview->second.operations.size() &&
            interview->second.operations[interview->second.operationIndex].transactionSequenceAssigned) {
            InterviewOperation& operation = interview->second.operations[interview->second.operationIndex];
            matched = (operation.type == InterviewAttributeRead || operation.type == InterviewBasicRead) &&
                      operation.transactionSequence == transactionSequence && operation.cluster == cluster &&
                      operation.endpoint == payload[5];
            if (matched && operation.type == InterviewBasicRead && updateBasicIdentity(&devices_[address->second], records))
                interview->second.persistentDirty = true;
        }
        if (applyZclReadAttributeResponse(&devices_[address->second], cluster, records, time(0))) {
            updateContactAlert(address->second, wasOpen);
            saveState();
        }
        if (matched) completeInterviewOperation(sender);
    } else if ((frameControl & 0x03) == 0 && command == 0x0a) {
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
    } else if ((frameControl & 0x03) == 0 && command == 0x07) {
        unsigned long responseOffset = zclOffset;
        while (responseOffset < message.size()) {
            unsigned char status = message[responseOffset];
            std::ostringstream response;
            response << "Configure reporting response from " << address->second << ": cluster 0x" << std::hex
                     << cluster << " status 0x" << static_cast<unsigned int>(status);
            ++responseOffset;
            if (status != 0 && responseOffset + 3 <= message.size()) {
                unsigned short attribute = static_cast<unsigned short>(message[responseOffset + 1]) |
                                           (static_cast<unsigned short>(message[responseOffset + 2]) << 8);
                response << " attribute 0x" << attribute;
                responseOffset += 3; /* direction plus attribute id */
            }
            addLog(response.str());
        }
        std::map<unsigned short, InterviewState>::iterator interview = interviews_.find(sender);
        if (interview != interviews_.end() &&
            interview->second.operationIndex < interview->second.operations.size() &&
            interview->second.operations[interview->second.operationIndex].transactionSequenceAssigned) {
            InterviewOperation& operation = interview->second.operations[interview->second.operationIndex];
            if (operation.type == InterviewConfigureReporting && operation.transactionSequence == transactionSequence &&
                operation.cluster == cluster && operation.endpoint == payload[5]) completeInterviewOperation(sender);
        }
    }
}
