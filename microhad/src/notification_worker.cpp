#include "notification_worker.h"
#include "fcm.h"
#include "web_push.h"
#include "json_util.h"

#include <cstdio>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool validId(const std::string& id) {
    if (id.size() != 36) return false;
    for (unsigned long i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != '-') return false; }
        else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    }
    return true;
}
bool printable(const std::string& value) {
    for (unsigned long i = 0; i < value.size(); ++i)
        if (static_cast<unsigned char>(value[i]) < 0x20 || value[i] == 0x7f) return false;
    return true;
}
}

NotificationWorker::NotificationWorker(const std::string& runtimeDirectory, const std::string& firebaseServiceAccount,
                                       const std::string& firebaseTopic, const std::string& caBundle,
                                       DeliveryHook* deliveryHook) :
    runtimeDirectory_(runtimeDirectory), firebaseServiceAccount_(firebaseServiceAccount), firebaseTopic_(firebaseTopic),
    caBundle_(caBundle), deliveryHook_(deliveryHook), running_(false), started_(false), enabled_(true) {
    pthread_mutex_init(&mutex_, 0);
    pthread_cond_init(&ready_, 0);
    loadEnabled();
    loadDevices();
}

NotificationWorker::~NotificationWorker() {
    stop();
    pthread_cond_destroy(&ready_);
    pthread_mutex_destroy(&mutex_);
}

bool NotificationWorker::start(std::string* error) {
    pthread_attr_t attributes;
    pthread_mutex_lock(&mutex_);
    if (started_) {
        pthread_mutex_unlock(&mutex_);
        *error = "notification worker already started";
        return false;
    }
    running_ = true;
    if (pthread_attr_init(&attributes) != 0) {
        running_ = false;
        pthread_mutex_unlock(&mutex_);
        *error = "cannot create notification worker";
        return false;
    }
    if (pthread_attr_setstacksize(&attributes, 131072) != 0 ||
        pthread_create(&thread_, &attributes, threadMain, this) != 0) {
        pthread_attr_destroy(&attributes);
        running_ = false;
        pthread_mutex_unlock(&mutex_);
        *error = "cannot create notification worker";
        return false;
    }
    pthread_attr_destroy(&attributes);
    started_ = true;
    pthread_mutex_unlock(&mutex_);
    return true;
}

void NotificationWorker::stop() {
    pthread_mutex_lock(&mutex_);
    if (!started_) {
        pthread_mutex_unlock(&mutex_);
        return;
    }
    running_ = false;
    pthread_cond_signal(&ready_);
    pthread_mutex_unlock(&mutex_);
    pthread_join(thread_, 0);
    pthread_mutex_lock(&mutex_);
    started_ = false;
    pthread_mutex_unlock(&mutex_);
}

bool NotificationWorker::enqueue(const std::string& title, const std::string& body, std::string* error) {
    Job job;
    pthread_mutex_lock(&mutex_);
    if (!running_) {
        pthread_mutex_unlock(&mutex_);
        *error = "notification worker is unavailable";
        return false;
    }
    if (!enabled_) {
        pthread_mutex_unlock(&mutex_);
        *error = "push notifications are disabled";
        return false;
    }
    if (jobs_.size() >= 32) {
        pthread_mutex_unlock(&mutex_);
        *error = "notification queue is full";
        return false;
    }
    job.title = title;
    job.body = body;
    jobs_.push_back(job);
    pthread_cond_signal(&ready_);
    pthread_mutex_unlock(&mutex_);
    return true;
}

bool NotificationWorker::enabled() const {
    pthread_mutex_lock(&mutex_);
    bool value = enabled_;
    pthread_mutex_unlock(&mutex_);
    return value;
}

bool NotificationWorker::setEnabled(bool enabled, std::string* error) {
    pthread_mutex_lock(&mutex_);
    if (enabled_ == enabled) {
        pthread_mutex_unlock(&mutex_);
        return true;
    }
    enabled_ = enabled;
    if (!enabled_) jobs_.clear();
    if (!saveEnabled(error)) {
        enabled_ = !enabled;
        pthread_mutex_unlock(&mutex_);
        return false;
    }
    pthread_mutex_unlock(&mutex_);
    return true;
}
void* NotificationWorker::threadMain(void* opaque) {
    static_cast<NotificationWorker*>(opaque)->run();
    return 0;
}

void NotificationWorker::run() {
    while (true) {
        Job job;
        pthread_mutex_lock(&mutex_);
        while (running_ && jobs_.empty()) pthread_cond_wait(&ready_, &mutex_);
        if (!running_ && jobs_.empty()) {
            pthread_mutex_unlock(&mutex_);
            break;
        }
        job = jobs_.front();
        jobs_.erase(jobs_.begin());
        pthread_mutex_unlock(&mutex_);
        deliver(job);
    }
}

void NotificationWorker::deliver(const Job& job) {
    std::string ignored;
    if (!enabled()) return;
    if (deliveryHook_ != 0) {
        deliveryHook_->deliver(job.title, job.body);
        return;
    }
    if (!firebaseServiceAccount_.empty() && !caBundle_.empty()) {
        std::vector<std::string> tokens;
        pthread_mutex_lock(&mutex_);
        for (unsigned long i = 0; i < devices_.size(); ++i)
            if (devices_[i].enabled) tokens.push_back(devices_[i].token);
        pthread_mutex_unlock(&mutex_);
        for (unsigned long i = 0; i < tokens.size() && enabled(); ++i)
            sendFcmTokenNotification(firebaseServiceAccount_, caBundle_, tokens[i], job.title, job.body, &ignored);
    }
    if (!enabled()) return;
    if (!caBundle_.empty()) {
        WebPushStore browserPush(runtimeDirectory_);
        browserPush.send(caBundle_, job.title, job.body, &ignored);
    }
}
bool NotificationWorker::registerDevice(const std::string& id, const std::string& model,
                                         const std::string& token, std::string* error) {
    if (!validId(id) || model.empty() || model.size() > 128 || !printable(model) ||
        token.empty() || token.size() > 4096 || !printable(token)) {
        *error = "invalid push device registration"; return false;
    }
    pthread_mutex_lock(&mutex_);
    unsigned long i = 0;
    while (i < devices_.size() && devices_[i].id != id) ++i;
    if (i == devices_.size() && devices_.size() >= 32) {
        pthread_mutex_unlock(&mutex_); *error = "push device limit reached"; return false;
    }
    if (i < devices_.size() && devices_[i].model == model && devices_[i].token == token) {
        pthread_mutex_unlock(&mutex_); return true;
    }
    std::vector<Device> old = devices_;
    if (i == devices_.size()) {
        Device device; device.id = id; device.model = model; device.token = token;
        device.label = ""; device.enabled = true; devices_.push_back(device);
    } else { devices_[i].model = model; devices_[i].token = token; }
    bool saved = saveDevices(error);
    if (!saved) devices_.swap(old);
    pthread_mutex_unlock(&mutex_);
    return saved;
}
bool NotificationWorker::updateDevice(const std::string& id, const std::string& label,
                                       bool enabled, std::string* error) {
    if (!validId(id) || label.size() > 64 || !printable(label)) {
        *error = "invalid push device settings"; return false;
    }
    pthread_mutex_lock(&mutex_);
    unsigned long i = 0;
    while (i < devices_.size() && devices_[i].id != id) ++i;
    if (i == devices_.size()) {
        pthread_mutex_unlock(&mutex_); *error = "push device not found"; return false;
    }
    if (devices_[i].label == label && devices_[i].enabled == enabled) {
        pthread_mutex_unlock(&mutex_); return true;
    }
    Device old = devices_[i];
    devices_[i].label = label; devices_[i].enabled = enabled;
    bool saved = saveDevices(error);
    if (!saved) devices_[i] = old;
    pthread_mutex_unlock(&mutex_);
    return saved;
}
std::string NotificationWorker::devicesJson() const {
    std::string json = "{\"devices\":[";
    pthread_mutex_lock(&mutex_);
    for (unsigned long i = 0; i < devices_.size(); ++i) {
        if (i) json += ",";
        json += "{\"id\":\"" + jsonEscape(devices_[i].id) + "\",\"model\":\"" +
                jsonEscape(devices_[i].model) + "\",\"label\":\"" + jsonEscape(devices_[i].label) +
                "\",\"enabled\":" + (devices_[i].enabled ? "true}" : "false}");
    }
    pthread_mutex_unlock(&mutex_);
    return json + "]}";
}
void NotificationWorker::loadDevices() {
    std::ifstream input((runtimeDirectory_ + "/push-devices.ndjson").c_str());
    std::string line;
    while (devices_.size() < 32 && std::getline(input, line)) {
        Device device; long enabled = 0;
        if (jsonStringField(line, "id", &device.id) && validId(device.id) &&
            jsonStringField(line, "model", &device.model) && !device.model.empty() && device.model.size() <= 128 &&
            jsonStringField(line, "token", &device.token) && !device.token.empty() && device.token.size() <= 4096 &&
            jsonStringField(line, "label", &device.label) && device.label.size() <= 64 &&
            jsonLongField(line, "enabled", &enabled) && (enabled == 0 || enabled == 1)) {
            device.enabled = enabled == 1;
            bool duplicate = false;
            for (unsigned long i = 0; i < devices_.size(); ++i) if (devices_[i].id == device.id) duplicate = true;
            if (!duplicate) devices_.push_back(device);
        }
    }
}
bool NotificationWorker::saveDevices(std::string* error) const {
    const std::string filename = runtimeDirectory_ + "/push-devices.ndjson";
    const std::string temporary = filename + ".tmp";
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { *error = "cannot save push devices"; return false; }
    if (fchmod(fd, 0600) != 0) { close(fd); unlink(temporary.c_str()); *error = "cannot save push devices"; return false; }
    FILE* output = fdopen(fd, "w");
    if (output == 0) { close(fd); unlink(temporary.c_str()); *error = "cannot save push devices"; return false; }
    bool ok = true;
    for (unsigned long i = 0; i < devices_.size(); ++i) {
        const Device& d = devices_[i];
        std::string line = "{\"id\":\"" + jsonEscape(d.id) + "\",\"model\":\"" + jsonEscape(d.model) +
            "\",\"token\":\"" + jsonEscape(d.token) + "\",\"label\":\"" + jsonEscape(d.label) +
            "\",\"enabled\":" + (d.enabled ? "1}\n" : "0}\n");
        if (fwrite(line.data(), 1, line.size(), output) != line.size()) { ok = false; break; }
    }
    if (fclose(output) != 0) ok = false;
    if (ok && rename(temporary.c_str(), filename.c_str()) == 0) return true;
    unlink(temporary.c_str()); *error = "cannot save push devices"; return false;
}
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
    if (!output) {
        *error = "cannot save notification setting";
        return false;
    }
    output << "{\"enabled\":" << (enabled_ ? "true" : "false") << "}\n";
    output.close();
    if (!output || rename(temporary.c_str(), filename.c_str()) != 0) {
        remove(temporary.c_str());
        *error = "cannot save notification setting";
        return false;
    }
    return true;
}
