#include "matter_worker.h"
#include "gateway_runner.h"
#include "thread_name.h"
#include <time.h>
#include <unistd.h>
#include <sstream>

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
}

MatterWorker::MatterWorker(GatewayRunner* gateway, const std::string& socketPath) :
    gateway_(gateway), client_(socketPath), nextJob_(1), running_(false), started_(false), refreshed_(0) {
    pthread_mutex_init(&mutex_, 0);
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
        if (value.state == "failed") out << ",\"error\":\"transport\"";
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
    client_.disconnect();
    pthread_mutex_lock(&mutex_);
    matterStatus_.clear(); threadStatus_.clear(); refreshed_ = 0; events_.clear();
    for (std::map<uint32_t, Job>::iterator it = jobs_.begin(); it != jobs_.end(); ++it) {
        if (it->second.state == "queued" || it->second.state == "running") {
            wipe(&it->second.body); it->second.state = "failed";
        }
    }
    pending_.clear(); pthread_mutex_unlock(&mutex_);
}
bool MatterWorker::collectEvents() {
    std::string event;
    while (client_.popEvent(&event)) {
        pthread_mutex_lock(&mutex_);
        bool full = events_.size() >= 8;
        if (!full) events_.push_back(event);
        pthread_mutex_unlock(&mutex_);
        if (full) { unavailable(); return false; }
    }
    return true;
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
    pthread_mutex_lock(&mutex_);
    Job& completed = jobs_[id]; completed.state = valid ? "complete" : "failed";
    completed.response = response;
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
            if (valid) valid = client_.call(1, "", &matter, &error);
            if (!running()) break;
            if (valid) valid = client_.call(2, "", &thread, &error);
            pthread_mutex_lock(&mutex_);
            if (valid) { matterStatus_ = matter; threadStatus_ = thread; refreshed_ = tick(); }
            pthread_mutex_unlock(&mutex_);
            if (!valid) unavailable(); else collectEvents();
        }
        if (!running()) break;
        executeJob();
        std::string error;
        if (!client_.poll(&error)) unavailable(); else collectEvents();
        usleep(10000);
    }
    unavailable();
}
