#include "gateway_runner.h"
#include "gateway.h"

#include <unistd.h>

GatewayRunner::GatewayRunner(Gateway* gateway) : gateway_(gateway), commandHead_(0), commandTail_(0), started_(false),
    startupComplete_(false), startupSucceeded_(false), stopping_(false), runtimeDirectory_(gateway->runtimeDirectory()) {
    pthread_mutex_init(&mutex_, 0);
    pthread_cond_init(&commandReady_, 0);
    pthread_cond_init(&startupReady_, 0);
}

GatewayRunner::~GatewayRunner() {
    stop();
    pthread_cond_destroy(&startupReady_);
    pthread_cond_destroy(&commandReady_);
    pthread_mutex_destroy(&mutex_);
    delete gateway_;
}

bool GatewayRunner::start(std::string* error) {
    pthread_mutex_lock(&mutex_);
    if (started_) { pthread_mutex_unlock(&mutex_); *error = "coordinator thread already started"; return false; }
    started_ = true;
    if (pthread_create(&thread_, 0, runThread, this) != 0) {
        started_ = false; pthread_mutex_unlock(&mutex_); *error = "cannot create coordinator thread"; return false;
    }
    while (!startupComplete_) pthread_cond_wait(&startupReady_, &mutex_);
    if (!startupSucceeded_) *error = startupError_;
    bool result = startupSucceeded_;
    pthread_mutex_unlock(&mutex_);
    return result;
}

void GatewayRunner::stop() {
    Command* pending;
    pthread_mutex_lock(&mutex_);
    if (!started_) { pthread_mutex_unlock(&mutex_); return; }
    stopping_ = true;
    pending = commandHead_;
    commandHead_ = 0;
    commandTail_ = 0;
    pthread_cond_broadcast(&commandReady_);
    pthread_mutex_unlock(&mutex_);
    while (pending != 0) {
        Command* next = pending->next;
        pthread_mutex_lock(&pending->mutex);
        pending->result = false;
        pending->error = "coordinator is stopping";
        pending->complete = true;
        pthread_cond_signal(&pending->ready);
        pthread_mutex_unlock(&pending->mutex);
        pending = next;
    }
    pthread_join(thread_, 0);
    pthread_mutex_lock(&mutex_);
    started_ = false;
    pthread_mutex_unlock(&mutex_);
}

void* GatewayRunner::runThread(void* opaque) {
    static_cast<GatewayRunner*>(opaque)->run();
    return 0;
}

void GatewayRunner::run() {
    std::string error;
    bool ready = gateway_->start(&error);
    pthread_mutex_lock(&mutex_);
    startupComplete_ = true;
    startupSucceeded_ = ready;
    startupError_ = error;
    pthread_cond_broadcast(&startupReady_);
    pthread_mutex_unlock(&mutex_);
    if (!ready) return;
    while (true) {
        Command* command = 0;
        pthread_mutex_lock(&mutex_);
        if (commandHead_ != 0) {
            command = commandHead_;
            commandHead_ = commandHead_->next;
            if (commandHead_ == 0) commandTail_ = 0;
        }
        bool stopNow = stopping_;
        pthread_mutex_unlock(&mutex_);
        if (command != 0) execute(command);
        if (stopNow) break;
        gateway_->poll();
        updateSnapshots();
        usleep(20000);
    }
    gateway_->stop();
}

void GatewayRunner::updateSnapshots() {
    std::string status = gateway_->statusJson();
    std::string logs = gateway_->logsJson();
    pthread_mutex_lock(&mutex_);
    statusJson_ = status;
    logsJson_ = logs;
    pthread_mutex_unlock(&mutex_);
}

void GatewayRunner::execute(Command* command) {
    std::string error;
    bool result = false;
    if (command->type == Join) result = gateway_->setJoining(command->seconds, &error);
    else if (command->type == CreateNetwork) result = gateway_->createFreshNetwork(&error);
    else if (command->type == DeviceSettings) result = gateway_->setDeviceSettings(command->address, command->name,
                                                                                      command->hasLow, command->low,
                                                                                      command->hasHigh, command->high,
                                                                                      command->hasContactTimings,
                                                                                      command->initialAlertMinutes,
                                                                                      command->reminderAlertMinutes, &error);
    else if (command->type == DeviceRefresh) result = gateway_->refreshDevice(command->address, &error);
    else if (command->type == TestNotification) result = gateway_->sendTestNotification(&error);
    else if (command->type == NotificationSetting) result = gateway_->setNotificationsEnabled(command->hasLow, &error);
    updateSnapshots();
    pthread_mutex_lock(&command->mutex);
    command->result = result;
    command->error = error;
    command->complete = true;
    pthread_cond_signal(&command->ready);
    pthread_mutex_unlock(&command->mutex);
}

bool GatewayRunner::invoke(Command* command, std::string* error) {
    pthread_mutex_init(&command->mutex, 0);
    pthread_cond_init(&command->ready, 0);
    command->complete = false;
    command->next = 0;
    pthread_mutex_lock(&mutex_);
    if (!started_ || stopping_ || !startupSucceeded_) {
        pthread_mutex_unlock(&mutex_);
        pthread_cond_destroy(&command->ready); pthread_mutex_destroy(&command->mutex);
        *error = "coordinator is unavailable";
        return false;
    }
    if (commandTail_ == 0) commandHead_ = command; else commandTail_->next = command;
    commandTail_ = command;
    pthread_cond_signal(&commandReady_);
    pthread_mutex_unlock(&mutex_);
    pthread_mutex_lock(&command->mutex);
    while (!command->complete) pthread_cond_wait(&command->ready, &command->mutex);
    bool result = command->result;
    *error = command->error;
    pthread_mutex_unlock(&command->mutex);
    pthread_cond_destroy(&command->ready); pthread_mutex_destroy(&command->mutex);
    return result;
}

std::string GatewayRunner::statusJson() const { pthread_mutex_lock(&mutex_); std::string value = statusJson_; pthread_mutex_unlock(&mutex_); return value; }
std::string GatewayRunner::logsJson() const { pthread_mutex_lock(&mutex_); std::string value = logsJson_; pthread_mutex_unlock(&mutex_); return value; }
std::string GatewayRunner::runtimeDirectory() const { return runtimeDirectory_; }

bool GatewayRunner::setJoining(unsigned int seconds, std::string* error) { Command command; command.type = Join; command.seconds = seconds; return invoke(&command, error); }
bool GatewayRunner::createFreshNetwork(std::string* error) { Command command; command.type = CreateNetwork; return invoke(&command, error); }
bool GatewayRunner::setDeviceSettings(const std::string& address, const std::string& name,
                                      bool hasLow, double low, bool hasHigh, double high,
                                      bool hasContactTimings, unsigned long initialAlertMinutes,
                                      unsigned long reminderAlertMinutes, std::string* error) {
    Command command;
    command.type = DeviceSettings;
    command.address = address;
    command.name = name;
    command.hasLow = hasLow;
    command.low = low;
    command.hasHigh = hasHigh;
    command.high = high;
    command.hasContactTimings = hasContactTimings;
    command.initialAlertMinutes = initialAlertMinutes;
    command.reminderAlertMinutes = reminderAlertMinutes;
    return invoke(&command, error);
}
bool GatewayRunner::refreshDevice(const std::string& address, std::string* error) {
    Command command;
    command.type = DeviceRefresh;
    command.address = address;
    return invoke(&command, error);
}
bool GatewayRunner::sendTestNotification(std::string* error) { Command command; command.type = TestNotification; return invoke(&command, error); }
bool GatewayRunner::setNotificationsEnabled(bool enabled, std::string* error) { Command command; command.type = NotificationSetting; command.hasLow = enabled; return invoke(&command, error); }
