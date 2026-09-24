#include "notification_worker.h"
#include "fcm.h"
#include "web_push.h"

#include <cstdio>
#include <fstream>

NotificationWorker::NotificationWorker(const std::string& runtimeDirectory, const std::string& firebaseServiceAccount,
                                       const std::string& firebaseTopic, const std::string& caBundle,
                                       DeliveryHook* deliveryHook) :
    runtimeDirectory_(runtimeDirectory), firebaseServiceAccount_(firebaseServiceAccount), firebaseTopic_(firebaseTopic),
    caBundle_(caBundle), deliveryHook_(deliveryHook), running_(false), started_(false), enabled_(true) { pthread_mutex_init(&mutex_, 0); pthread_cond_init(&ready_, 0); loadEnabled(); }
NotificationWorker::~NotificationWorker() { stop(); pthread_cond_destroy(&ready_); pthread_mutex_destroy(&mutex_); }
bool NotificationWorker::start(std::string* error) {
    pthread_attr_t attributes;
    pthread_mutex_lock(&mutex_);
    if (started_) { pthread_mutex_unlock(&mutex_); *error = "notification worker already started"; return false; }
    running_ = true;
    if (pthread_attr_init(&attributes) != 0 || pthread_attr_setstacksize(&attributes, 131072) != 0 ||
        pthread_create(&thread_, &attributes, threadMain, this) != 0) {
        pthread_attr_destroy(&attributes); running_ = false; pthread_mutex_unlock(&mutex_); *error = "cannot create notification worker"; return false;
    }
    pthread_attr_destroy(&attributes);
    started_ = true; pthread_mutex_unlock(&mutex_); return true;
}
void NotificationWorker::stop() { pthread_mutex_lock(&mutex_); if (!started_) { pthread_mutex_unlock(&mutex_); return; } running_ = false; pthread_cond_signal(&ready_); pthread_mutex_unlock(&mutex_); pthread_join(thread_, 0); pthread_mutex_lock(&mutex_); started_ = false; pthread_mutex_unlock(&mutex_); }
bool NotificationWorker::enqueue(const std::string& title, const std::string& body, std::string* error) {
    Job job; pthread_mutex_lock(&mutex_);
    if (!running_) { pthread_mutex_unlock(&mutex_); *error = "notification worker is unavailable"; return false; }
    if (!enabled_) { pthread_mutex_unlock(&mutex_); *error = "push notifications are disabled"; return false; }
    if (jobs_.size() >= 32) { pthread_mutex_unlock(&mutex_); *error = "notification queue is full"; return false; }
    job.title = title; job.body = body; jobs_.push_back(job); pthread_cond_signal(&ready_); pthread_mutex_unlock(&mutex_); return true;
}
bool NotificationWorker::enabled() const { pthread_mutex_lock(&mutex_); bool value = enabled_; pthread_mutex_unlock(&mutex_); return value; }
bool NotificationWorker::setEnabled(bool enabled, std::string* error) {
    pthread_mutex_lock(&mutex_);
    if (enabled_ == enabled) { pthread_mutex_unlock(&mutex_); return true; }
    enabled_ = enabled;
    if (!enabled_) jobs_.clear();
    if (!saveEnabled(error)) { enabled_ = !enabled; pthread_mutex_unlock(&mutex_); return false; }
    pthread_mutex_unlock(&mutex_);
    return true;
}
void* NotificationWorker::threadMain(void* opaque) { static_cast<NotificationWorker*>(opaque)->run(); return 0; }
void NotificationWorker::run() { while (true) { Job job; pthread_mutex_lock(&mutex_); while (running_ && jobs_.empty()) pthread_cond_wait(&ready_, &mutex_); if (!running_ && jobs_.empty()) { pthread_mutex_unlock(&mutex_); break; } job = jobs_.front(); jobs_.erase(jobs_.begin()); pthread_mutex_unlock(&mutex_); deliver(job); } }
void NotificationWorker::deliver(const Job& job) { std::string ignored; if (!enabled()) return; if (deliveryHook_ != 0) { deliveryHook_->deliver(job.title, job.body); return; } if (!firebaseServiceAccount_.empty() && !firebaseTopic_.empty() && !caBundle_.empty()) sendFcmNotification(firebaseServiceAccount_, caBundle_, firebaseTopic_, job.title, job.body, &ignored); if (!enabled()) return; if (!caBundle_.empty()) { WebPushStore browserPush(runtimeDirectory_); browserPush.send(caBundle_, job.title, job.body, &ignored); } }
void NotificationWorker::loadEnabled() {
    std::ifstream input((runtimeDirectory_ + "/notifications.json").c_str());
    std::string content;
    if (!input) return;
    content.assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (content.find("\"enabled\":false") != std::string::npos) enabled_ = false;
}
bool NotificationWorker::saveEnabled(std::string* error) const {
    std::string filename = runtimeDirectory_ + "/notifications.json";
    std::string temporary = filename + ".tmp";
    std::ofstream output(temporary.c_str());
    if (!output) { *error = "cannot save notification setting"; return false; }
    output << "{\"enabled\":" << (enabled_ ? "true" : "false") << "}\n";
    output.close();
    if (!output || rename(temporary.c_str(), filename.c_str()) != 0) { remove(temporary.c_str()); *error = "cannot save notification setting"; return false; }
    return true;
}
