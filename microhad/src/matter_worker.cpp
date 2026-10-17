#include "matter_worker.h"
#include "gateway_runner.h"
#include "json_util.h"
#include "thread_name.h"
#include <time.h>
#include <unistd.h>
#include <sstream>
#include <fcntl.h>
#include <sys/socket.h>
#include <syslog.h>
#include <cerrno>
#include <algorithm>

namespace {
int64_t tick() {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ?
        static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000 : 0;
}
void wipe(std::string* value) {
    if (!value->empty()) {
        volatile char* bytes = &(*value)[0];
        for (unsigned long i = 0; i < value->size(); ++i) bytes[i] = 0;
    }
    value->clear();
}
bool sameReadingKey(const MatterReading& left, const MatterReading& right) {
    return left.node == right.node && left.endpoint == right.endpoint &&
        left.cluster == right.cluster;
}
const size_t MaxDeferredMatterReadings = 128;
}

MatterWorker::MatterWorker(GatewayRunner* gateway, const std::string& socketPath) :
    gateway_(gateway), phonePendingFd_(-1), phoneActive_(false), phoneFd_(-1), phoneReady_(false), phoneOffset_(0),
    phoneReceived_(0), phoneWriteSince_(0), phoneLastId_(0), phoneGeneration_(0), client_(socketPath),
    nextJob_(1), running_(false), started_(false), refreshed_(0) {
    pthread_mutex_init(&mutex_, 0);
    client_.setEventHandler(&MatterWorker::drainMatterEvents, this);
}
MatterWorker::~MatterWorker() { stop(); pthread_mutex_destroy(&mutex_); }

bool MatterWorker::start(std::string* error) {
    pthread_mutex_lock(&mutex_);
    if (started_) { pthread_mutex_unlock(&mutex_); *error = "Matter worker already started"; return false; }
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
        pthread_mutex_unlock(&mutex_); *error = "Cannot create Matter worker"; return false;
    }
    running_ = true;
    if (pthread_attr_setstacksize(&attributes, 65536) != 0 ||
        pthread_create(&thread_, &attributes, threadMain, this) != 0) {
        running_ = false; pthread_attr_destroy(&attributes);
        pthread_mutex_unlock(&mutex_); *error = "Cannot create Matter worker"; return false;
    }
    started_ = true; pthread_attr_destroy(&attributes);
    pthread_mutex_unlock(&mutex_); return true;
}
void MatterWorker::stop() {
    pthread_mutex_lock(&mutex_);
    if (!started_) { pthread_mutex_unlock(&mutex_); return; }
    running_ = false;
    pthread_mutex_unlock(&mutex_);
    pthread_join(thread_, 0);
    pthread_mutex_lock(&mutex_);
    for (std::map<uint32_t, Job>::iterator it = jobs_.begin(); it != jobs_.end(); ++it) wipe(&it->second.body);
    jobs_.clear(); pending_.clear(); events_.clear();
    deferredReadings_.clear();
    started_ = false; refreshed_ = 0; matterStatus_.clear(); threadStatus_.clear();
    pthread_mutex_unlock(&mutex_);
}
bool MatterWorker::running() {
    pthread_mutex_lock(&mutex_); bool value = running_; pthread_mutex_unlock(&mutex_); return value;
}
bool MatterWorker::status(bool thread, std::string* json, std::string* error) {
    const int64_t now = tick();
    json->clear(); error->clear();
    pthread_mutex_lock(&mutex_);
    bool fresh = running_ && refreshed_ > 0 && now >= refreshed_ && now - refreshed_ <= 5000;
    if (fresh) *json = thread ? threadStatus_ : matterStatus_;
    pthread_mutex_unlock(&mutex_);
    if (json->empty()) { *error = "Matter service unavailable"; return false; }
    return true;
}
void* MatterWorker::threadMain(void* opaque) {
    setCurrentThreadName("matter-ipc"); static_cast<MatterWorker*>(opaque)->run(); return 0;
}

bool MatterWorker::submit(unsigned char opcode, const std::string& body, uint32_t* id, std::string* error) {
    *id = 0; error->clear();
    if (!MatterChannel::validRequest(opcode, body)) { *error = "malformed"; return false; }
    pthread_mutex_lock(&mutex_);
    const int64_t now = tick();
    if (!running_ || refreshed_ <= 0 || now < refreshed_ || now - refreshed_ > 5000) {
        pthread_mutex_unlock(&mutex_); *error = "not_ready"; return false;
    }
    if (pending_.size() >= 8 || nextJob_ == 0) {
        pthread_mutex_unlock(&mutex_); *error = "capacity"; return false;
    }
    if (jobs_.size() >= 16) {
        std::map<uint32_t, Job>::iterator old = jobs_.begin();
        while (old != jobs_.end() && old->second.state != "complete" && old->second.state != "failed") ++old;
        if (old == jobs_.end()) { pthread_mutex_unlock(&mutex_); *error = "capacity"; return false; }
        jobs_.erase(old);
    }
    *id = nextJob_++;
    Job& value = jobs_[*id]; value.opcode = opcode; value.body = body; value.state = "queued";
    pending_.push_back(*id);
    pthread_mutex_unlock(&mutex_); return true;
}
bool MatterWorker::job(uint32_t id, std::string* json) {
    json->clear();
    pthread_mutex_lock(&mutex_);
    std::map<uint32_t, Job>::const_iterator found = jobs_.find(id);
    if (found != jobs_.end()) {
        const Job& value = found->second;
        std::ostringstream out;
        out << "{\"job\":" << id << ",\"state\":\"" << value.state << "\"";
        if (!value.response.empty()) out << ",\"response\":" << value.response;
        if (value.state == "failed") out << ",\"error\":\"" << (value.error.empty() ? "transport" : value.error) << '"';
        out << '}'; *json = out.str();
    }
    pthread_mutex_unlock(&mutex_); return !json->empty();
}
bool MatterWorker::takeEvent(std::string* json) {
    json->clear(); pthread_mutex_lock(&mutex_);
    if (!events_.empty()) { *json = events_.front(); events_.pop_front(); }
    pthread_mutex_unlock(&mutex_); return !json->empty();
}
void MatterWorker::unavailable() {
    dropPhone(false, 1011);
    client_.disconnect();
    std::vector<MatterEndpoint> offline;
    pthread_mutex_lock(&mutex_);
    matterStatus_.clear(); threadStatus_.clear(); refreshed_ = 0; events_.clear();
    offline = snapshotDevices_;
    bool wasOnline = false;
    for (unsigned long i = 0; i < offline.size(); ++i) {
        wasOnline = wasOnline || offline[i].online;
        offline[i].online = false;
        offline[i].metadataComplete = false;
    }
    if (wasOnline) snapshotDevices_ = offline;
    else offline.clear();
    inventoryBuilding_.clear();
    for (std::map<uint32_t, Job>::iterator it = jobs_.begin(); it != jobs_.end(); ++it) {
        if (it->second.state == "queued" || it->second.state == "running") {
            wipe(&it->second.body); it->second.state = "failed"; it->second.error = "transport";
        }
    }
    pending_.clear(); pthread_mutex_unlock(&mutex_);
    if (!offline.empty() && gateway_) {
        std::string ignored;
        gateway_->reconcileMatterDevices(offline, &ignored);
    }
}
bool MatterWorker::collectEvents() {
    std::string event;
    while (client_.popEvent(&event)) {
        std::vector<std::string> nodes;
        if (MatterChannel::inventoryNodesEvent(event, &nodes)) {
            std::string error;
            if (gateway_ && !gateway_->reconcileMatterNodes(nodes, &error)) { unavailable(); return false; }
            for (std::map<std::string, std::vector<MatterEndpoint> >::iterator it = inventoryBuilding_.begin(); it != inventoryBuilding_.end();)
                if (std::find(nodes.begin(), nodes.end(), it->first) == nodes.end()) inventoryBuilding_.erase(it++); else ++it;
            for (std::vector<MatterEndpoint>::iterator it = snapshotDevices_.begin(); it != snapshotDevices_.end();)
                if (std::find(nodes.begin(), nodes.end(), it->node) == nodes.end()) it = snapshotDevices_.erase(it); else ++it;
            for (std::deque<MatterReading>::iterator it = deferredReadings_.begin(); it != deferredReadings_.end();)
                if (std::find(nodes.begin(), nodes.end(), it->node) == nodes.end()) it = deferredReadings_.erase(it); else ++it;
            continue;
        }
        std::string kind; unsigned int count = 0; MatterEndpoint endpoint;
        if (MatterChannel::inventoryEvent(event, &kind, &count, &endpoint)) {
            std::vector<MatterEndpoint> update;
            if (kind == "identity") {
                inventoryBuilding_[endpoint.node].clear();
                update.push_back(endpoint);
            } else if (kind == "endpoint") {
                std::vector<MatterEndpoint>& building = inventoryBuilding_[endpoint.node];
                if (building.size() < 32) building.push_back(endpoint);
            } else {
                std::map<std::string, std::vector<MatterEndpoint> >::iterator building = inventoryBuilding_.find(endpoint.node);
                if (building != inventoryBuilding_.end() && building->second.size() == count) {
                    update.swap(building->second); update.push_back(endpoint);
                    inventoryBuilding_.erase(building);
                }
            }
            if (!update.empty() && gateway_) {
                std::string error;
                if (gateway_->reconcileMatterDevices(update, &error)) {
                    if (kind == "commit" && endpoint.metadataComplete) {
                        for (std::vector<MatterEndpoint>::iterator it=snapshotDevices_.begin();it!=snapshotDevices_.end();) {
                            if (it->node == endpoint.node) it=snapshotDevices_.erase(it); else ++it;
                        }
                    }
                    for (unsigned long i=0;i<update.size();++i) {
                        bool found = false;
                        for (unsigned long j=0;j<snapshotDevices_.size();++j) if (snapshotDevices_[j].id == update[i].id) {
                            snapshotDevices_[j] = update[i]; found = true; break;
                        }
                        if (!found) snapshotDevices_.push_back(update[i]);
                    }
                    flushDeferredReadings();
                } else syslog(LOG_WARNING, "Matter inventory rejected: %s", error.c_str());
            }
            continue;
        }
        MatterReading reading;
        if (MatterChannel::deviceReportEvent(event, &reading)) {
            applyOrDeferReading(reading);
            continue;
        }
        MatterButtonEvent button;
        if (MatterChannel::deviceButtonEvent(event, &button)) {
            if (gateway_) {
                std::string ignored;
                gateway_->applyMatterButtonEvent(button, &ignored);
            }
            continue;
        }
        pthread_mutex_lock(&mutex_);
        bool full = events_.size() >= 32;
        if (!full) events_.push_back(event);
        pthread_mutex_unlock(&mutex_);
        if (full) {
            syslog(LOG_WARNING, "Matter worker disconnect: reason=event_queue_full queued=32");
            unavailable(); return false;
        }
    }
    return true;
}

bool MatterWorker::drainMatterEvents(void* opaque) {
    MatterWorker* worker = static_cast<MatterWorker*>(opaque);
    return worker != 0 && worker->collectEvents();
}

void MatterWorker::applyOrDeferReading(const MatterReading& reading) {
    if (!gateway_) return;
    std::string error;
    if (gateway_->applyMatterReading(reading, &error) || error != "Matter endpoint not found") return;
    for (std::deque<MatterReading>::iterator it = deferredReadings_.begin();
         it != deferredReadings_.end(); ++it) {
        if (sameReadingKey(*it, reading)) {
            *it = reading;
            return;
        }
    }
    if (deferredReadings_.size() == MaxDeferredMatterReadings) deferredReadings_.pop_front();
    deferredReadings_.push_back(reading);
}

void MatterWorker::flushDeferredReadings() {
    if (!gateway_) {
        deferredReadings_.clear();
        return;
    }
    for (std::deque<MatterReading>::iterator it = deferredReadings_.begin();
         it != deferredReadings_.end();) {
        std::string error;
        if (gateway_->applyMatterReading(*it, &error) || error != "Matter endpoint not found")
            it = deferredReadings_.erase(it);
        else ++it;
    }
}
void MatterWorker::executeJob() {
    pthread_mutex_lock(&mutex_);
    if (pending_.empty()) { pthread_mutex_unlock(&mutex_); return; }
    uint32_t id = pending_.front(); pending_.pop_front();
    Job& value = jobs_[id]; value.state = "running";
    unsigned char opcode = value.opcode;
    std::string body; body.swap(value.body);
    pthread_mutex_unlock(&mutex_);
    std::string response, error;
    bool valid;
    if (opcode == 9) {
        unsigned char channel = 0; bool online = false;
        if (gateway_) gateway_->coordinatorObservation(&channel, &online);
        std::string request;
        if (!online || !MatterChannel::threadChannel(body, channel, &request)) {
            response = "{\"schema\":1,\"ok\":false,\"error\":\"not_ready\"}";
            valid = true;
        } else valid = client_.call(9, request, &response, &error);
        wipe(&request);
    } else valid = client_.call(opcode, body, &response, &error);
    wipe(&body);
    std::string serviceJob;
    const bool operation = valid && MatterChannel::acceptedOperationJob(opcode, response, &serviceJob);
    pthread_mutex_lock(&mutex_);
    Job& completed = jobs_[id];
    if (operation) {
        completed.state = "running";
        completed.serviceJob = serviceJob;
        completed.pollAt = tick() + 200;
    } else {
        completed.state = valid ? "complete" : "failed";
        completed.error = valid ? "" : "transport";
        completed.response = response;
    }
    pthread_mutex_unlock(&mutex_);
    if (!valid) unavailable(); else collectEvents();
}
void MatterWorker::pollOperationJob() {
    uint32_t id = 0;
    std::string serviceJob;
    const int64_t now = tick();
    pthread_mutex_lock(&mutex_);
    for (std::map<uint32_t, Job>::iterator it = jobs_.begin(); it != jobs_.end(); ++it) {
        if (it->second.state == "running" && !it->second.serviceJob.empty() && it->second.pollAt <= now) {
            id = it->first; serviceJob = it->second.serviceJob; it->second.pollAt = now + 200; break;
        }
    }
    pthread_mutex_unlock(&mutex_);
    if (id == 0) return;
    const std::string body = MatterChannel::operationPollRequest(serviceJob);
    std::string response, error;
    bool valid = !body.empty() && client_.call(10, body, &response, &error);
    bool terminal = false, failed = false;
    std::string operationError;
    if (valid) valid = MatterChannel::operationStatus(response, serviceJob, &terminal, &failed, &operationError);
    MatterReading reading;
    if (valid && terminal && !failed) {
        valid = MatterChannel::deviceResult(response, serviceJob, &reading);
        if (valid && gateway_) {
            std::string ignored;
            if (reading.cluster == 62) {
                if (!gateway_->removeMatterNode(reading.node, &ignored)) { failed = true; operationError = "storage"; }
            } else gateway_->applyMatterReading(reading, &ignored);
        }
    }
    pthread_mutex_lock(&mutex_);
    std::map<uint32_t, Job>::iterator found = jobs_.find(id);
    if (found != jobs_.end() && found->second.state == "running" && found->second.serviceJob == serviceJob) {
        if (!valid) {
            found->second.state = "failed"; found->second.error = "transport";
        } else if (terminal) {
            found->second.state = failed ? "failed" : "complete";
            found->second.error = operationError;
            found->second.response = response;
            found->second.serviceJob.clear();
        }
    }
    pthread_mutex_unlock(&mutex_);
    if (!valid) unavailable(); else collectEvents();
}
void MatterWorker::run() {
    int64_t next = 0;
    while (running()) {
        if (tick() >= next) {
            next = tick() + 1000;
            unsigned char channel = 0; bool online = false;
            if (gateway_) gateway_->coordinatorObservation(&channel, &online);
            std::ostringstream body;
            body << "{\"schema\":1,\"action\":\"observe\",\"zigbee_channel\":"
                 << static_cast<unsigned int>(channel) << ",\"zigbee_online\":" << (online ? "true" : "false") << '}';
            std::string response, error, matter, thread;
            bool valid = client_.call(9, body.str(), &response, &error);
            if (!running()) break;
            if (valid && !abandonedSession_.empty()) {
                const std::string abandoned = abandonedSession_;
                const std::string cancel = "{\"schema\":1,\"session\":\"" + abandoned + "\"}";
                valid = client_.call(5, cancel, &response, &error);
                if (valid) abandonedSession_.clear();
            }
            if (valid) valid = client_.call(1, "", &matter, &error);
            if (!running()) break;
            if (valid) valid = client_.call(2, "", &thread, &error);
            pthread_mutex_lock(&mutex_);
            if (valid) { matterStatus_ = matter; threadStatus_ = thread; refreshed_ = tick(); }
            pthread_mutex_unlock(&mutex_);
            if (!valid) unavailable(); else collectEvents();
        }
        if (!running()) break;
        phonePump();
        if (!running()) break;
        executeJob();
        if (!running()) break;
        pollOperationJob();
        std::string error;
        if (!client_.poll(&error)) unavailable(); else collectEvents();
        usleep(10000);
    }
    dropPhone(true, 1001);
    unavailable();
}

bool MatterWorker::adoptRelay(int fd, const std::string& handshake, std::string* error) {
    error->clear(); pthread_mutex_lock(&mutex_);
    const int64_t now = tick();
    if (!running_ || refreshed_ <= 0 || now < refreshed_ || now - refreshed_ > 5000) {
        pthread_mutex_unlock(&mutex_); *error = "not_ready"; return false;
    }
    if (phoneActive_) { pthread_mutex_unlock(&mutex_); *error = "busy"; return false; }
    int owned = dup(fd);
    if (owned < 0 || fcntl(owned, F_SETFD, FD_CLOEXEC) != 0 || fcntl(owned, F_SETFL, O_NONBLOCK) != 0) {
        if (owned >= 0) close(owned);
        pthread_mutex_unlock(&mutex_); *error = "transport"; return false;
    }
    phoneActive_ = true; phonePendingFd_ = owned; phoneHandshake_ = handshake;
    pthread_mutex_unlock(&mutex_); return true;
}
bool MatterWorker::phoneQueue(unsigned char opcode, const std::string& payload) {
    MatterWebSocket::Message message; message.opcode = opcode; message.payload = payload;
    std::string frame;
    if (phoneOutput_.size() >= 8 || !MatterWebSocket::encode(message, &frame)) return false;
    if (phoneOutput_.empty()) phoneWriteSince_ = tick();
    phoneOutput_.push_back(frame); return true;
}
void MatterWorker::dropPhone(bool cancel, unsigned int code) {
    std::string session; session.swap(phoneSession_);
    if (phoneFd_ >= 0) {
        // A close frame is best-effort; never append it to a partially sent frame.
        if (phoneReady_ && (phoneOutput_.empty() || phoneOffset_ == 0)) {
            MatterWebSocket::Message message; message.opcode = 8;
            message.payload += static_cast<char>(code >> 8); message.payload += static_cast<char>(code);
            std::string frame;
            if (MatterWebSocket::encode(message, &frame)) send(phoneFd_, frame.data(), frame.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        }
        close(phoneFd_); phoneFd_ = -1;
    }
    phoneDecoder_ = MatterWebSocket::Decoder(); phoneOutput_.clear(); phoneOffset_ = 0;
    phoneLastId_ = 0; phoneGeneration_ = 0; phoneReady_ = false;
    pthread_mutex_lock(&mutex_);
    if (phonePendingFd_ >= 0) { close(phonePendingFd_); phonePendingFd_ = -1; }
    phoneHandshake_.clear(); phoneActive_ = false;
    pthread_mutex_unlock(&mutex_);
    if (!session.empty() && !cancel) abandonedSession_ = session;
    if (cancel && !session.empty()) {
        std::string response, error;
        if (!client_.call(5, "{\"schema\":1,\"session\":\"" + session + "\"}", &response, &error)) {
            abandonedSession_ = session;
            unavailable();
        } else if (abandonedSession_ == session) abandonedSession_.clear();
    }
}
void MatterWorker::phonePump() {
    pthread_mutex_lock(&mutex_);
    if (phonePendingFd_ >= 0) {
        phoneFd_ = phonePendingFd_; phonePendingFd_ = -1;
        phoneOutput_.push_back(phoneHandshake_); phoneHandshake_.clear(); phoneWriteSince_ = tick();
        phoneReceived_ = tick(); events_.clear();
    }
    pthread_mutex_unlock(&mutex_);
    if (phoneFd_ < 0) return;
    const int64_t now = tick();
    if (now - phoneReceived_ > 30000 || (!phoneOutput_.empty() && now - phoneWriteSince_ > 2000)) {
        dropPhone(true, 1008); return;
    }
    char buffer[MatterWebSocket::MaxChunk];
    ssize_t count = recv(phoneFd_, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (count == 0 || (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        dropPhone(true, 1001); return;
    }
    if (count > 0 && !phoneReady_) { dropPhone(true, 1002); return; }
    if (count > 0 && !phoneDecoder_.feed(std::string(buffer, count))) {
        dropPhone(true, phoneDecoder_.closeCode()); return;
    }
    MatterWebSocket::Message message;
    if (phoneDecoder_.pop(&message)) {
        phoneReceived_ = tick();
        if (message.opcode == 8) { dropPhone(true, 1000); return; }
        if (message.opcode == 9) {
            if (!phoneQueue(10, message.payload)) { dropPhone(true, 1009); return; }
        } else if (message.opcode == 1) {
            uint32_t id; unsigned char opcode; std::string session, body;
            if (!MatterChannel::relayRequest(message.payload, &id, &opcode, &session, &body) || id <= phoneLastId_ ||
                (phoneSession_.empty() && opcode != 4) || (!phoneSession_.empty() && session != phoneSession_)) {
                dropPhone(true, 1008); return;
            }
            phoneLastId_ = id;
            std::string response, error;
            if (!client_.call(opcode, body, &response, &error)) { unavailable(); return; }
            if (opcode == 4 && MatterChannel::relayResponseSession(response) == session) phoneSession_ = session;
            if (opcode == 6) { uint32_t generation = MatterChannel::relayGeneration(response); if (generation) phoneGeneration_ = generation; }
            std::ostringstream out; out << "{\"schema\":1,\"id\":" << id << ",\"response\":" << response << '}';
            if (!phoneQueue(1, out.str())) { dropPhone(true, 1009); return; }
            if (!collectEvents()) return;
        }
    }
    std::string event;
    while (takeEvent(&event)) {
        if (MatterChannel::relayEvent(event, phoneSession_, phoneGeneration_) && !phoneQueue(1, event)) {
            dropPhone(true, 1009); return;
        }
    }
    if (!phoneOutput_.empty()) {
        const std::string& frame = phoneOutput_.front();
        ssize_t written = send(phoneFd_, frame.data() + phoneOffset_, frame.size() - phoneOffset_, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (written > 0) {
            phoneOffset_ += written;
            if (phoneOffset_ == frame.size()) { phoneOutput_.pop_front(); phoneReady_ = true; phoneOffset_ = 0; phoneWriteSince_ = tick(); }
        } else if (written < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) dropPhone(true, 1001);
    }
}
