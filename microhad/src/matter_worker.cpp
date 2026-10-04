#include "matter_worker.h"
#include "gateway_runner.h"
#include "thread_name.h"
#include <time.h>
#include <unistd.h>

namespace {
int64_t tick() {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ?
        static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000 : 0;
}
}

MatterWorker::MatterWorker(GatewayRunner* gateway, const std::string& socketPath) :
    gateway_(gateway), client_(socketPath), running_(false), started_(false), refreshed_(0) {
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
void MatterWorker::run() {
    while (running()) {
        int64_t next = tick() + 1000;
        unsigned char channel = 0; bool online = false;
        if (gateway_) gateway_->coordinatorObservation(&channel, &online);
        std::string response, error, matter, thread;
        bool valid = client_.observe(channel, online, &response, &error);
        if (!running()) break;
        if (valid) valid = client_.status(false, &matter, &error);
        if (!running()) break;
        if (valid) valid = client_.status(true, &thread, &error);
        pthread_mutex_lock(&mutex_);
        if (valid) { matterStatus_ = matter; threadStatus_ = thread; refreshed_ = tick(); }
        else { matterStatus_.clear(); threadStatus_.clear(); refreshed_ = 0; }
        pthread_mutex_unlock(&mutex_);
        while (running() && tick() < next) usleep(50000);
    }
}
