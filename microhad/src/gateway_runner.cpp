#include "gateway_runner.h"
#include "gateway.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>

GatewayRunner::GatewayRunner(Gateway* gateway) : gateway_(gateway), commandWakeRead_(-1), commandWakeWrite_(-1),
    commandHead_(0), commandTail_(0), started_(false), startupComplete_(false), startupSucceeded_(false), stopping_(false),
    currentView_(0), runtimeDirectory_(gateway->runtimeDirectory()), historyPath_(gateway->historyPath()) {
    int wakePipe[2];
    pthread_mutex_init(&mutex_, 0);
    pthread_cond_init(&startupReady_, 0);
    if (pipe(wakePipe) == 0) {
        commandWakeRead_ = wakePipe[0]; commandWakeWrite_ = wakePipe[1];
        fcntl(commandWakeRead_, F_SETFL, fcntl(commandWakeRead_, F_GETFL) | O_NONBLOCK);
        fcntl(commandWakeWrite_, F_SETFL, fcntl(commandWakeWrite_, F_GETFL) | O_NONBLOCK);
    }
}

GatewayRunner::~GatewayRunner() {
    stop();
    if (currentView_ != 0) { delete currentView_->view; delete currentView_; }
    if (commandWakeRead_ >= 0) close(commandWakeRead_);
    if (commandWakeWrite_ >= 0) close(commandWakeWrite_);
    pthread_cond_destroy(&startupReady_);
    pthread_mutex_destroy(&mutex_);
    delete gateway_;
}

bool GatewayRunner::start(std::string* error) {
    pthread_mutex_lock(&mutex_);
    if (started_) { pthread_mutex_unlock(&mutex_); *error = "coordinator thread already started"; return false; }
    if (commandWakeRead_ < 0 || commandWakeWrite_ < 0) { pthread_mutex_unlock(&mutex_); *error = "cannot create coordinator wakeup pipe"; return false; }
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
    wakeCoordinator();
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
    if (ready) publishView(true);
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
        publishView(false);
        waitForWork();
    }
    gateway_->stop();
}

void GatewayRunner::publishView(bool force) {
    if (!force && !gateway_->takeViewDirty()) return;
    GatewayView* view = new GatewayView(gateway_->captureView());
    if (force) gateway_->takeViewDirty();
    pthread_mutex_lock(&mutex_);
    ViewData* previous = currentView_;
    currentView_ = new ViewData(view);
    if (previous != 0) {
        previous->retired = true;
        if (previous->references == 0) { delete previous->view; delete previous; }
    }
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
                                                                                      command->reminderAlertMinutes,
                                                                                      command->hasSensorRefreshInterval,
                                                                                      command->sensorRefreshIntervalMinutes, &error);
    else if (command->type == DeviceRefresh) result = gateway_->refreshDevice(command->address, &error);
    else if (command->type == DeviceIdentify) result = gateway_->identifyDevice(command->address, &error);
    else if (command->type == LightState) result = gateway_->setLightState(command->address, command->name,
                                                                             command->brightnessPercent, &error);
    else if (command->type == SceneConfiguration) result = gateway_->setSceneConfiguration(command->configuration, &error);
    else if (command->type == SceneActivate) result = gateway_->activateScene(command->groupId, command->sceneId, &error);
    else if (command->type == SceneOff) result = gateway_->turnOffSceneGroup(command->groupId, &error);
    else if (command->type == TestNotification) result = gateway_->sendTestNotification(&error);
    else if (command->type == NotificationSetting) result = gateway_->setNotificationsEnabled(command->hasLow, &error);
    publishView(true);
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
    wakeCoordinator();
    pthread_mutex_unlock(&mutex_);
    pthread_mutex_lock(&command->mutex);
    while (!command->complete) pthread_cond_wait(&command->ready, &command->mutex);
    bool result = command->result;
    *error = command->error;
    pthread_mutex_unlock(&command->mutex);
    pthread_cond_destroy(&command->ready); pthread_mutex_destroy(&command->mutex);
    return result;
}

void GatewayRunner::wakeCoordinator() {
    char signal = 'w';
    if (commandWakeWrite_ >= 0) {
        ssize_t written = write(commandWakeWrite_, &signal, 1);
        (void)written; /* A full nonblocking pipe already carries a wakeup. */
    }
}

void GatewayRunner::waitForWork() {
    fd_set readable;
    fd_set writable;
    struct timeval timeout;
    struct timeval* timeoutPointer = 0;
    int serial = gateway_->serialFd();
    int maximum = commandWakeRead_;
    long deadline;
    pthread_mutex_lock(&mutex_);
    bool commandQueued = commandHead_ != 0 || stopping_;
    pthread_mutex_unlock(&mutex_);
    /* Pipe bytes are deliberately coalesced when drained.  Do not sleep if
       another queued command remains after the previous loop iteration. */
    if (commandQueued) return;
    if (gateway_->hasImmediateWork()) return;
    FD_ZERO(&readable); FD_ZERO(&writable);
    FD_SET(commandWakeRead_, &readable);
    if (serial >= 0) {
        FD_SET(serial, &readable);
        if (gateway_->serialNeedsWrite()) FD_SET(serial, &writable);
        if (serial > maximum) maximum = serial;
    }
    deadline = gateway_->nextPollDeadline();
    if (deadline != 0) {
        long seconds = deadline - time(0);
        if (seconds < 0) seconds = 0;
        timeout.tv_sec = seconds; timeout.tv_usec = 0;
        timeoutPointer = &timeout;
    }
    if (select(maximum + 1, &readable, &writable, 0, timeoutPointer) > 0 && FD_ISSET(commandWakeRead_, &readable)) {
        char buffer[64];
        while (read(commandWakeRead_, buffer, sizeof(buffer)) > 0) {}
    }
}

GatewayRunner::ViewLease::ViewLease(GatewayRunner* owner, ViewData* data) : data_(data), owner_(owner) {}
GatewayRunner::ViewLease::~ViewLease() { owner_->releaseView(data_); }
std::string GatewayRunner::ViewLease::statusJson() const { return data_->view->statusJson(); }
std::string GatewayRunner::ViewLease::logsJson() const { return data_->view->logsJson(); }

GatewayRunner::ViewLease* GatewayRunner::acquireView() {
    pthread_mutex_lock(&mutex_);
    ViewData* data = currentView_;
    if (data != 0) ++data->references;
    pthread_mutex_unlock(&mutex_);
    return data == 0 ? 0 : new ViewLease(this, data);
}

void GatewayRunner::releaseView(ViewData* data) {
    pthread_mutex_lock(&mutex_);
    --data->references;
    if (data->retired && data->references == 0) { delete data->view; delete data; }
    pthread_mutex_unlock(&mutex_);
}
std::string GatewayRunner::runtimeDirectory() const { return runtimeDirectory_; }
std::string GatewayRunner::historyPath() const { return historyPath_; }

bool GatewayRunner::setJoining(unsigned int seconds, std::string* error) { Command command; command.type = Join; command.seconds = seconds; return invoke(&command, error); }
bool GatewayRunner::createFreshNetwork(std::string* error) { Command command; command.type = CreateNetwork; return invoke(&command, error); }
bool GatewayRunner::setDeviceSettings(const std::string& address, const std::string& name,
                                      bool hasLow, double low, bool hasHigh, double high,
                                      bool hasContactTimings, unsigned long initialAlertMinutes,
                                      unsigned long reminderAlertMinutes, bool hasSensorRefreshInterval,
                                      unsigned long sensorRefreshIntervalMinutes, std::string* error) {
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
    command.hasSensorRefreshInterval = hasSensorRefreshInterval;
    command.sensorRefreshIntervalMinutes = sensorRefreshIntervalMinutes;
    return invoke(&command, error);
}
bool GatewayRunner::refreshDevice(const std::string& address, std::string* error) {
    Command command;
    command.type = DeviceRefresh;
    command.address = address;
    return invoke(&command, error);
}
bool GatewayRunner::identifyDevice(const std::string& address, std::string* error) {
    Command command;
    command.type = DeviceIdentify;
    command.address = address;
    return invoke(&command, error);
}
bool GatewayRunner::setLightState(const std::string& address, const std::string& state, int brightnessPercent,
                                  std::string* error) {
    Command command;
    command.type = LightState;
    command.address = address;
    command.name = state;
    command.brightnessPercent = brightnessPercent;
    return invoke(&command, error);
}
bool GatewayRunner::setSceneConfiguration(const std::string& configuration, std::string* error) {
    Command command; command.type = SceneConfiguration; command.configuration = configuration; return invoke(&command, error);
}
bool GatewayRunner::activateScene(unsigned short groupId, unsigned char sceneId, std::string* error) {
    Command command; command.type = SceneActivate; command.groupId = groupId; command.sceneId = sceneId; return invoke(&command, error);
}
bool GatewayRunner::turnOffSceneGroup(unsigned short groupId, std::string* error) {
    Command command; command.type = SceneOff; command.groupId = groupId; return invoke(&command, error);
}
bool GatewayRunner::sendTestNotification(std::string* error) { Command command; command.type = TestNotification; return invoke(&command, error); }
bool GatewayRunner::setNotificationsEnabled(bool enabled, std::string* error) { Command command; command.type = NotificationSetting; command.hasLow = enabled; return invoke(&command, error); }
