#include "notification_worker.h"
#include "fcm.h"
#include "web_push.h"

NotificationWorker::NotificationWorker(const std::string& runtimeDirectory, const std::string& firebaseServiceAccount,
                                       const std::string& firebaseTopic, const std::string& caBundle) :
    runtimeDirectory_(runtimeDirectory), firebaseServiceAccount_(firebaseServiceAccount), firebaseTopic_(firebaseTopic),
    caBundle_(caBundle), running_(false), started_(false) { pthread_mutex_init(&mutex_, 0); pthread_cond_init(&ready_, 0); }
NotificationWorker::~NotificationWorker() { stop(); pthread_cond_destroy(&ready_); pthread_mutex_destroy(&mutex_); }
bool NotificationWorker::start(std::string* error) {
    pthread_mutex_lock(&mutex_);
    if (started_) { pthread_mutex_unlock(&mutex_); *error = "notification worker already started"; return false; }
    running_ = true;
    if (pthread_create(&thread_, 0, threadMain, this) != 0) { running_ = false; pthread_mutex_unlock(&mutex_); *error = "cannot create notification worker"; return false; }
    started_ = true; pthread_mutex_unlock(&mutex_); return true;
}
void NotificationWorker::stop() { pthread_mutex_lock(&mutex_); if (!started_) { pthread_mutex_unlock(&mutex_); return; } running_ = false; pthread_cond_signal(&ready_); pthread_mutex_unlock(&mutex_); pthread_join(thread_, 0); pthread_mutex_lock(&mutex_); started_ = false; pthread_mutex_unlock(&mutex_); }
bool NotificationWorker::enqueue(const std::string& title, const std::string& body, std::string* error) {
    Job job; pthread_mutex_lock(&mutex_);
    if (!running_) { pthread_mutex_unlock(&mutex_); *error = "notification worker is unavailable"; return false; }
    if (jobs_.size() >= 32) { pthread_mutex_unlock(&mutex_); *error = "notification queue is full"; return false; }
    job.title = title; job.body = body; jobs_.push_back(job); pthread_cond_signal(&ready_); pthread_mutex_unlock(&mutex_); return true;
}
void* NotificationWorker::threadMain(void* opaque) { static_cast<NotificationWorker*>(opaque)->run(); return 0; }
void NotificationWorker::run() { while (true) { Job job; pthread_mutex_lock(&mutex_); while (running_ && jobs_.empty()) pthread_cond_wait(&ready_, &mutex_); if (!running_ && jobs_.empty()) { pthread_mutex_unlock(&mutex_); break; } job = jobs_.front(); jobs_.erase(jobs_.begin()); pthread_mutex_unlock(&mutex_); deliver(job); } }
void NotificationWorker::deliver(const Job& job) { std::string ignored; if (!firebaseServiceAccount_.empty() && !firebaseTopic_.empty() && !caBundle_.empty()) sendFcmNotification(firebaseServiceAccount_, caBundle_, firebaseTopic_, job.title, job.body, &ignored); if (!caBundle_.empty()) { WebPushStore browserPush(runtimeDirectory_); browserPush.send(caBundle_, job.title, job.body, &ignored); } }
