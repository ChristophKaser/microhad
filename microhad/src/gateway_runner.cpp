#include "gateway_runner.h"
#include "automation_profile.h"
#include "gateway.h"
#include "thread_name.h"
#include "automation_worker.h"

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <sstream>
#include <sys/select.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <syslog.h>
#endif

GatewayRunner::LoopDebugStats::LoopDebugStats() : iterations(0), commandSkips(0), immediateSkips(0),
    selectCalls(0), selectReady(0), selectTimeouts(0), selectInterrupted(0), selectErrors(0),
    serialReadReady(0), serialWriteReady(0), wakeReady(0), lastSelectResult(0), lastSelectErrno(0),
    lastTimeoutSeconds(-1), lastDeadline(0), lastDeadlineSource("none") {
}

GatewayRunner::GatewayRunner(Gateway* gateway) : gateway_(gateway), automation_(0), automationChain_(0), activeAutomationChain_(0), activeAutomationDepth_(0), historyStore_(gateway->historyStore()), commandWakeRead_(-1), commandWakeWrite_(-1),
    commandHead_(0), commandTail_(0), started_(false), startupComplete_(false), startupSucceeded_(false), stopping_(false),
    eventLoopDebug_(false), currentView_(0), lastCoordinatorProgress_(0), runtimeDirectory_(gateway->runtimeDirectory()), historyPath_(gateway->historyPath()) {
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
    setCurrentThreadName("coordinator");
#if defined(__linux__)
    // Linux nice values are per thread. Keep normal scheduling so the radio
    // services and automation callbacks can still make progress under load.
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), -20) != 0)
        syslog(LOG_WARNING, "Cannot raise coordinator priority: errno=%d", errno);
#endif
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
    LoopDebugStats debugStats;
    time_t nextDebugReportAt = 0;
    while (true) {
        Command* command = 0;
        Automation::Action action;
        bool hasAction = false;
        pthread_mutex_lock(&mutex_);
        if (commandHead_ != 0) {
            command = commandHead_;
            commandHead_ = commandHead_->next;
            if (commandHead_ == 0) commandTail_ = 0;
        }
        if (!automationActions_.empty()) { action=automationActions_.front(); automationActions_.pop_front(); hasAction=true; }
        bool stopNow = stopping_;
        pthread_mutex_unlock(&mutex_);
        if (command != 0) execute(command);
        if (stopNow) break;
        if (hasAction) executeAutomation(action);
        if (!eventLoopDebug_) {
            debugStats = LoopDebugStats();
            nextDebugReportAt = 0;
        } else {
            if (nextDebugReportAt == 0) nextDebugReportAt = time(0) + 5;
            ++debugStats.iterations;
        }
        { AUTOMATION_PROFILE("coord.poll");gateway_->poll(); }
        publishView(false);
        struct timespec progress;
        pthread_mutex_lock(&mutex_);
        lastCoordinatorProgress_ = clock_gettime(CLOCK_MONOTONIC, &progress) == 0 ?
            static_cast<int64_t>(progress.tv_sec) * 1000 + progress.tv_nsec / 1000000 : 0;
        pthread_mutex_unlock(&mutex_);
        waitForWork(&debugStats, eventLoopDebug_);
        if (eventLoopDebug_ && time(0) >= nextDebugReportAt) {
            std::ostringstream report;
            report << "EVENT_LOOP_DEBUG interval_s=5 loops=" << debugStats.iterations
                   << " command_skips=" << debugStats.commandSkips
                   << " immediate_skips=" << debugStats.immediateSkips
                   << " select_calls=" << debugStats.selectCalls
                   << " select_ready=" << debugStats.selectReady
                   << " select_timeouts=" << debugStats.selectTimeouts
                   << " select_eintr=" << debugStats.selectInterrupted
                   << " select_errors=" << debugStats.selectErrors
                   << " serial_read_ready=" << debugStats.serialReadReady
                   << " serial_write_ready=" << debugStats.serialWriteReady
                   << " wake_ready=" << debugStats.wakeReady
                   << " last_select_result=" << debugStats.lastSelectResult
                   << " last_errno=" << debugStats.lastSelectErrno
                   << " last_timeout_s=" << debugStats.lastTimeoutSeconds
                   << " last_deadline=" << debugStats.lastDeadline
                   << " deadline_source=" << debugStats.lastDeadlineSource;
            gateway_->logEventLoopDiagnostic(report.str());
            debugStats = LoopDebugStats();
            nextDebugReportAt = time(0) + 5;
        }
    }
    gateway_->stop();
}

void GatewayRunner::publishView(bool force) {
    AUTOMATION_PROFILE("coord.publish");
    if (!force && !gateway_->takeViewDirty()) return;
#ifdef MICROHAD_TESTING
    GatewayView* view = new GatewayView(gateway_->captureView());
#else
    GatewayView* view = new GatewayView(gateway_->captureView(false));
#endif
    if (force) gateway_->takeViewDirty();
    publishAutomation(*view);
    pthread_mutex_lock(&mutex_);
    ViewData* previous = currentView_;
    currentView_ = new ViewData(view);
    if (previous != 0) {
        previous->retired = true;
        if (previous->references == 0) { delete previous->view; delete previous; }
    }
    pthread_mutex_unlock(&mutex_);
}

void GatewayRunner::setAutomationWorker(AutomationWorker* worker) { pthread_mutex_lock(&mutex_);automation_=worker;pthread_mutex_unlock(&mutex_); }
void GatewayRunner::automationGeneration(const std::string& script,unsigned long generation) { pthread_mutex_lock(&mutex_);automationGenerations_[script]=generation;pthread_mutex_unlock(&mutex_); }
bool GatewayRunner::submitAutomation(const Automation::Action& action,std::string* error) {
    if(action.operation!="actor"&&action.operation!="light"&&action.operation!="refresh"&&action.operation!="scene.off"&&action.operation!="scene.activate"){*error="unsupported automation action";return false;}
    pthread_mutex_lock(&mutex_);
    if(!started_||stopping_||!startupSucceeded_||!action.generation||automationGenerations_[action.script]!=action.generation||action.depth>32||automationActions_.size()>=128){pthread_mutex_unlock(&mutex_);*error="coordinator unavailable, retired generation, causal limit, or action queue full";return false;}
    AutomationProfile::mark("enqueue",action.target.c_str());automationActions_.push_back(action);wakeCoordinator();pthread_mutex_unlock(&mutex_);return true;
}
void GatewayRunner::executeAutomation(const Automation::Action& a) {
    AUTOMATION_PROFILE("coord.action");
    AutomationProfile::mark("dequeue",a.target.c_str());
    pthread_mutex_lock(&mutex_);
    const bool current=a.generation!=0&&automationGenerations_[a.script]==a.generation;
    pthread_mutex_unlock(&mutex_);
    if(!current)return;
    std::string error;bool ok=false;
    activeAutomationChain_=a.chain;activeAutomationDepth_=a.depth;
    if(a.operation=="actor")ok=gateway_->setActorState(a.target,a.args.fields.find("on")->second.boolean,&error);
    else if(a.operation=="light"){
        std::map<std::string,Automation::Value>::const_iterator on=a.args.fields.find("on"),brightness=a.args.fields.find("brightness");
        ok=gateway_->setLightState(a.target,on!=a.args.fields.end()&&on->second.boolean?"on":"off",brightness==a.args.fields.end()?-1:static_cast<int>(brightness->second.number),&error);
        if(ok&&on!=a.args.fields.end()&&brightness!=a.args.fields.end())ok=gateway_->setLightState(a.target,on->second.boolean?"on":"off",-1,&error);
    }else if(a.operation=="refresh")ok=gateway_->refreshDevice(a.target,&error);
    else if(a.operation=="scene.off")ok=gateway_->turnOffSceneGroup(static_cast<unsigned short>(a.args.fields.find("group")->second.number),&error);
    else if(a.operation=="scene.activate")ok=gateway_->activateScene(static_cast<unsigned short>(a.args.fields.find("group")->second.number),static_cast<unsigned char>(a.args.fields.find("scene")->second.number),&error);
    if(!ok)gateway_->logEventLoopDiagnostic("Automation action failed: "+error);
    // Correlate immediate changes to the accepted command; later radio reports
    // start a new observation chain because protocols do not echo our IDs.
    automationChain_=std::max(automationChain_,a.chain);
    publishView(true);
    activeAutomationChain_=0;activeAutomationDepth_=0;
}
void GatewayRunner::publishAutomation(const GatewayView& view) {
    AUTOMATION_PROFILE("coord.events");
    typedef Automation::Value V;
    std::vector<Gateway::AutomationReport> reports=gateway_->takeAutomationReports();
    std::map<std::string,V> next;
    std::vector<Automation::Event> outgoing;
    uint64_t chain=activeAutomationChain_?activeAutomationChain_:++automationChain_;
    // Preserve accepted reports and their intermediate edges even when several
    // arrive during one coordinator poll before the view is published.
    for(size_t i=0;i<reports.size();++i){V snapshot=Automation::snapshot(reports[i].device);const std::string id=snapshot.fields["id"].string;
        if(reports[i].remoteAction){Automation::Event e;e.data.fields["type"]=V("remote.action");e.data.fields["device"]=snapshot;e.data.fields["action"]=V(reports[i].action);e.data.fields["endpoint"]=V(static_cast<double>(reports[i].endpoint));if(reports[i].position>=0)e.data.fields["position"]=V(static_cast<double>(reports[i].position));if(reports[i].pressCount>=0)e.data.fields["press_count"]=V(static_cast<double>(reports[i].pressCount));outgoing.push_back(e);continue;}
        const std::string field=reports[i].field;Automation::Event e;e.data.fields["type"]=V("device.value");e.data.fields["device"]=snapshot;e.data.fields["field"]=V(field);e.data.fields["value"]=snapshot.fields[field];outgoing.push_back(e);
        if(automationSnapshots_.count(id)){V old=automationSnapshots_[id].fields[field];if(old!=snapshot.fields[field]){e.data.fields["type"]=V("device.changed");e.data.fields["old"]=old;outgoing.push_back(e);}automationSnapshots_[id].fields[field]=snapshot.fields[field];}
    }
    for(size_t i=0;i<view.devices.size();++i){V snapshot=Automation::snapshot(view.devices[i]);std::string id=snapshot.fields["id"].string;next[id]=snapshot;
        std::map<std::string,V>::iterator before=automationSnapshots_.find(id);
        if(before==automationSnapshots_.end()){Automation::Event e;e.data.fields["type"]=V("device.added");e.data.fields["device"]=snapshot;outgoing.push_back(e);}
        else for(std::map<std::string,V>::const_iterator f=snapshot.fields.begin();f!=snapshot.fields.end();++f){std::map<std::string,V>::const_iterator old=before->second.fields.find(f->first);if(old!=before->second.fields.end()&&old->second==f->second)continue;if(f->first=="last_seen")continue;Automation::Event e;e.data.fields["type"]=V("device.changed");e.data.fields["device"]=snapshot;e.data.fields["field"]=V(f->first);e.data.fields["value"]=f->second;e.data.fields["old"]=old==before->second.fields.end()?V():old->second;outgoing.push_back(e);if(f->first=="online"){e.data.fields["type"]=V(f->second.boolean?"device.online":"device.offline");outgoing.push_back(e);}}
    }
    for(std::map<std::string,V>::const_iterator it=automationSnapshots_.begin();it!=automationSnapshots_.end();++it)if(!next.count(it->first)){Automation::Event e;e.data.fields["type"]=V("device.removed");e.data.fields["device"]=it->second;e.data.fields["reason"]=V("removed");outgoing.push_back(e);}
    automationSnapshots_.swap(next);
    pthread_mutex_lock(&mutex_);
    if(automation_)for(size_t i=0;i<outgoing.size();++i){Automation::Event& e=outgoing[i];e.chain=chain;e.depth=activeAutomationDepth_;e.data.fields["device_id"]=e.data.fields["device"].fields["id"];e.data.fields["source"]=e.data.fields["device"].fields["source"];e.data.fields["at"]=V(static_cast<double>(time(0)));std::ostringstream id;id<<chain;e.data.fields["chain_id"]=V(id.str());e.data.fields["chain_depth"]=V(static_cast<double>(e.depth));automation_->enqueue(e);}
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
                                                                                      command->sensorRefreshIntervalMinutes, &error,
                                                                                      command->hasContactTriggerSettings,
                                                                                      command->contactTriggerMode,
                                                                                      command->contactTriggerCooldownSeconds);
    else if (command->type == DeviceValueAlerts) result = gateway_->setDeviceValueAlerts(command->address, command->valueAlerts, &error);
    else if (command->type == DeviceDelete) result = gateway_->deleteDevice(command->address, command->enabled, &error);
    else if (command->type == DeviceDeleteStatus) result = gateway_->deviceDeletionStatus(command->address, &command->configuration, &error);
    else if (command->type == MatterDeleteTarget) result = gateway_->matterDeletionTarget(command->address, &command->name, &error);
    else if (command->type == MatterNodeRemove) result = gateway_->removeMatterNode(command->address, &error);
    else if (command->type == MatterNodesReconcile) result = gateway_->reconcileMatterNodes(command->ids, &error);
    else if (command->type == DeviceRefresh) result = gateway_->refreshDevice(command->address, &error);
    else if (command->type == DeviceReconfigure) result = gateway_->reconfigureDevice(command->address, &error);
    else if (command->type == DeviceIdentify) result = gateway_->identifyDevice(command->address, &error);
    else if (command->type == NetworkBackup) result = gateway_->exportNetworkBackup(command->address, &error);
    else if (command->type == DeviceConfiguration) result = gateway_->queryDeviceConfiguration(command->address, &error);
    else if (command->type == ThermostatSetting) { std::string value=command->configuration;
        result = gateway_->setThermostat(command->address, command->name, value, &error, &command->configuration); }
    else if (command->type == ThermostatResult) result=gateway_->thermostatResult(command->matterReading.node,command->matterReading.endpoint,command->name,command->configuration);
    else if (command->type == LightState) result = gateway_->setLightState(command->address, command->name,
                                                                             command->brightnessPercent, &error);
    else if (command->type == SceneConfiguration) result = gateway_->setSceneConfiguration(command->configuration, &error);
    else if (command->type == SceneActivate) result = gateway_->activateScene(command->groupId, command->sceneId, &error);
    else if (command->type == SceneOff) result = gateway_->turnOffSceneGroup(command->groupId, &error);
    else if (command->type == TestNotification) result = gateway_->sendTestNotification(&error);
    else if (command->type == NotificationSetting) result = gateway_->setNotificationsEnabled(command->hasLow, &error);
    else if (command->type == ShellyRegister) result = gateway_->registerShelly(command->address, command->name,
                                                                                 command->oldName, command->model,
                                                                                 command->inputIndices,
                                                                                 command->temperatureIndices,
                                                                                 command->relayIndices, &error);
    else if (command->type == ShellyRemove) result = gateway_->removeShelly(command->address, &error);
    else if (command->type == ShellyReconcile) result = gateway_->reconcileShelly(command->ids, &error);
    else if (command->type == MatterReconcile) result = gateway_->reconcileMatterDevices(command->matterEndpoints, &error);
    else if (command->type == MatterValue) result = gateway_->applyMatterReading(command->matterReading, &error);
    else if (command->type == MatterButton) result = gateway_->applyMatterButtonEvent(command->matterButton, &error);
    else if (command->type == ShellyEvent) result = gateway_->applyShellyEvent(command->address, command->metric,
                                                                                command->channelIndex, command->value, &error);
    else if (command->type == FritzEvent) result = gateway_->applyFritzEvent(command->address, command->on,
                                                                               command->enabled, command->metric, &error);
    else if (command->type == FritzReconcile) result = gateway_->reconcileFritzClients(command->ids, &error);
    else if (command->type == ActorState) result = gateway_->setActorState(command->address, command->on, &error);
    else if (command->type == GatewaySettings) result = gateway_->setBroadcastQuorum(command->seconds, &error);
    else if (command->type == MatterRegionSettings)
        result = gateway_->setMatterCommissioningRegion(command->address, command->name, &error);
    else if (command->type == EventLoopDebug) {
        if (eventLoopDebug_ != command->enabled) {
            eventLoopDebug_ = command->enabled;
            gateway_->logEventLoopDiagnostic(std::string("Event-loop diagnostics ") +
                                             (eventLoopDebug_ ? "enabled" : "disabled"));
        }
        result = true;
    }
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

void GatewayRunner::waitForWork(LoopDebugStats* stats, bool debugEnabled) {
    fd_set readable;
    fd_set writable;
    struct timeval timeout;
    struct timeval* timeoutPointer = 0;
    int serial = gateway_->serialFd();
    int maximum = commandWakeRead_;
    long deadline;
    pthread_mutex_lock(&mutex_);
    bool commandQueued = commandHead_ != 0 || !automationActions_.empty() || stopping_;
    pthread_mutex_unlock(&mutex_);
    /* Pipe bytes are deliberately coalesced when drained.  Do not sleep if
       another queued command remains after the previous loop iteration. */
    if (commandQueued) {
        if (debugEnabled) ++stats->commandSkips;
        return;
    }
    if (gateway_->hasImmediateWork()) {
        if (debugEnabled) ++stats->immediateSkips;
        return;
    }
    FD_ZERO(&readable); FD_ZERO(&writable);
    FD_SET(commandWakeRead_, &readable);
    if (serial >= 0) {
        FD_SET(serial, &readable);
        if (gateway_->serialNeedsWrite()) FD_SET(serial, &writable);
        if (serial > maximum) maximum = serial;
    }
    if (debugEnabled) {
        stats->lastDeadline = gateway_->nextPollDeadline(&stats->lastDeadlineSource);
    } else {
        deadline = gateway_->nextPollDeadline();
    }
    if (debugEnabled) deadline = stats->lastDeadline;
    if (deadline != 0) {
        long seconds = deadline - time(0);
        if (seconds < 0) seconds = 0;
        timeout.tv_sec = seconds; timeout.tv_usec = 0;
        timeoutPointer = &timeout;
    }
    if (debugEnabled && (timeoutPointer == 0 || timeout.tv_sec > 5)) {
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        timeoutPointer = &timeout;
    }
#if MICROHAD_MATTER_ENABLED
    // Refresh observation liveness even when an otherwise idle coordinator
    // has no Zigbee deadline. The Matter worker never invokes Gateway itself.
    if (timeoutPointer == 0 || timeout.tv_sec > 1) {
        timeout.tv_sec = 1; timeout.tv_usec = 0; timeoutPointer = &timeout;
    }
#endif
    const long cacheDelay = gateway_->deviceCacheWaitMilliseconds();
    if (cacheDelay >= 0 && (timeoutPointer == 0 || cacheDelay < timeout.tv_sec * 1000 + timeout.tv_usec / 1000)) {
        timeout.tv_sec=cacheDelay/1000;timeout.tv_usec=(cacheDelay%1000)*1000;timeoutPointer=&timeout;
    }
    if (debugEnabled) {
        ++stats->selectCalls;
        stats->lastTimeoutSeconds = timeoutPointer == 0 ? -1 : static_cast<int>(timeout.tv_sec);
    }
    int selectResult = select(maximum + 1, &readable, &writable, 0, timeoutPointer);
    int selectError = selectResult < 0 ? errno : 0;
    if (debugEnabled) {
        stats->lastSelectResult = selectResult;
        stats->lastSelectErrno = selectError;
        if (selectResult == 0) ++stats->selectTimeouts;
        else if (selectResult > 0) {
            ++stats->selectReady;
            if (serial >= 0 && FD_ISSET(serial, &readable)) ++stats->serialReadReady;
            if (serial >= 0 && FD_ISSET(serial, &writable)) ++stats->serialWriteReady;
            if (FD_ISSET(commandWakeRead_, &readable)) ++stats->wakeReady;
        } else if (selectError == EINTR) ++stats->selectInterrupted;
        else ++stats->selectErrors;
    }
    if (selectResult > 0 && FD_ISSET(commandWakeRead_, &readable)) {
        char buffer[64];
        while (read(commandWakeRead_, buffer, sizeof(buffer)) > 0) {}
    }
}

GatewayRunner::ViewLease::ViewLease(GatewayRunner* owner, ViewData* data) : data_(data), owner_(owner) {}
GatewayRunner::ViewLease::~ViewLease() { owner_->releaseView(data_); }
std::string GatewayRunner::ViewLease::settingsJson() const { return data_->view->settingsJson; }
std::string GatewayRunner::ViewLease::matterCountryCode() const { return data_->view->matterCountryCode; }
std::string GatewayRunner::ViewLease::matterRegulatoryLocation() const { return data_->view->matterRegulatoryLocation; }
std::string GatewayRunner::ViewLease::statusJson() const { return data_->view->statusJson(); }
#ifdef MICROHAD_TESTING
std::string GatewayRunner::ViewLease::logsJson() const { return data_->view->logsJson(); }
#endif
long GatewayRunner::ViewLease::serialOpenedAt() const { return data_->view->serialOpenedAt; }
bool GatewayRunner::ViewLease::coordinatorOnline() const {
    return data_->view->online && data_->view->networkFormed &&
           data_->view->observedNetworkChannel >= 11 && data_->view->observedNetworkChannel <= 26;
}
unsigned char GatewayRunner::ViewLease::coordinatorChannel() const {
    return coordinatorOnline() ? data_->view->observedNetworkChannel : 0;
}
std::vector<Device> GatewayRunner::ViewLease::devices() const { return data_->view->devices; }
std::vector<SceneGroup> GatewayRunner::ViewLease::sceneGroups() const { return data_->view->sceneGroups; }

GatewayRunner::ViewLease* GatewayRunner::acquireView() {
    pthread_mutex_lock(&mutex_);
    ViewData* data = currentView_;
    if (data != 0) ++data->references;
    pthread_mutex_unlock(&mutex_);
    return data == 0 ? 0 : new ViewLease(this, data);
}

void GatewayRunner::coordinatorObservation(unsigned char* channel, bool* online) {
    struct timespec now;
    int64_t tick = clock_gettime(CLOCK_MONOTONIC, &now) == 0 ?
        static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000 : 0;
    *channel = 0; *online = false;
    pthread_mutex_lock(&mutex_);
    if (started_ && !stopping_ && startupSucceeded_ && currentView_ != 0 &&
        lastCoordinatorProgress_ > 0 && tick >= lastCoordinatorProgress_ &&
        tick - lastCoordinatorProgress_ <= 2500) {
        const GatewayView& view = *currentView_->view;
        if (view.online && view.networkFormed && view.observedNetworkChannel >= 11 &&
            view.observedNetworkChannel <= 26) {
            *channel = view.observedNetworkChannel; *online = true;
        }
    }
    pthread_mutex_unlock(&mutex_);
}

void GatewayRunner::releaseView(ViewData* data) {
    pthread_mutex_lock(&mutex_);
    --data->references;
    if (data->retired && data->references == 0) { delete data->view; delete data; }
    pthread_mutex_unlock(&mutex_);
}
std::string GatewayRunner::runtimeDirectory() const { return runtimeDirectory_; }
std::string GatewayRunner::historyPath() const { return historyPath_; }
#ifdef MICROHAD_TESTING
std::string GatewayRunner::logsJson() const {
    ViewLease* view = const_cast<GatewayRunner*>(this)->acquireView();
    if (view == 0) return "{\"lines\":[]}";
    std::string json = view->logsJson();
    delete view;
    return json;
}
#endif
bool GatewayRunner::readHistoryJson(const std::string& address, uint64_t before,
                                    std::string* json, std::string* error, uint32_t since) const {
    std::vector<std::string> historyIds(1, address);
    if (address.size()==40 && address.substr(0,7)=="matter:") {
        ViewLease* lease=const_cast<GatewayRunner*>(this)->acquireView();
        if (lease) {
            const std::vector<Device>& devices=lease->data_->view->devices;
            for (unsigned long index=0;index<devices.size();++index) {
                const Device& device=devices[index];
                if (device.source=="matter" && device.ieeeAddress.substr(0,40)==address &&
                    device.matterEndpoint>=0 &&
                    std::find(historyIds.begin(), historyIds.end(), device.ieeeAddress)==historyIds.end())
                    historyIds.push_back(device.ieeeAddress);
            }
            delete lease;
        }
    }
    return historyStore_ != 0 && historyStore_->readForDevices(historyIds, before, json, error, 0, since);
}

bool GatewayRunner::registerShelly(const std::string& id, const std::string& name, const std::string& oldName,
                                   const std::string& model, const std::vector<int>& inputIndices,
                                   const std::vector<int>& temperatureIndices, const std::vector<int>& relayIndices,
                                   std::string* error) {
    Command command; command.type = ShellyRegister; command.address = id; command.name = name;
    command.oldName = oldName; command.model = model; command.inputIndices = inputIndices;
    command.temperatureIndices = temperatureIndices; command.relayIndices = relayIndices;
    return invoke(&command, error);
}
bool GatewayRunner::removeShelly(const std::string& id, std::string* error) {
    Command command; command.type = ShellyRemove; command.address = id; return invoke(&command, error);
}
bool GatewayRunner::reconcileShelly(const std::vector<std::string>& ids, std::string* error) {
    Command command; command.type = ShellyReconcile; command.ids = ids; return invoke(&command, error);
}
bool GatewayRunner::reconcileMatterDevices(const std::vector<MatterEndpoint>& endpoints, std::string* error) {
    Command command; command.type = MatterReconcile; command.matterEndpoints = endpoints; return invoke(&command, error);
}
bool GatewayRunner::applyMatterReading(const MatterReading& reading, std::string* error) {
    Command command; command.type = MatterValue; command.matterReading = reading; return invoke(&command, error);
}
bool GatewayRunner::applyMatterButtonEvent(const MatterButtonEvent& event, std::string* error) {
    Command command; command.type = MatterButton; command.matterButton = event; return invoke(&command, error);
}
bool GatewayRunner::reconcileFritzClients(const std::vector<std::string>& ids, std::string* error) {
    Command command; command.type = FritzReconcile; command.ids = ids; return invoke(&command, error);
}
bool GatewayRunner::applyShellyEvent(const std::string& id, int metric, int channelIndex, double value, std::string* error) {
    Command command; command.type = ShellyEvent; command.address = id; command.metric = metric; command.value = value;
    command.channelIndex = channelIndex;
    return invoke(&command, error);
}
bool GatewayRunner::applyFritzEvent(const std::string& id, bool connected, bool hasSignal, int signalPercent, std::string* error) {
    Command command; command.type = FritzEvent; command.address = id; command.on = connected;
    command.enabled = hasSignal; command.metric = signalPercent; return invoke(&command, error);
}
bool GatewayRunner::setActorState(const std::string& address, bool on, std::string* error) {
    Command command; command.type = ActorState; command.address = address; command.on = on; return invoke(&command, error);
}

bool GatewayRunner::setJoining(unsigned int seconds, std::string* error) { Command command; command.type = Join; command.seconds = seconds; return invoke(&command, error); }
bool GatewayRunner::createFreshNetwork(std::string* error) { Command command; command.type = CreateNetwork; return invoke(&command, error); }
bool GatewayRunner::setDeviceSettings(const std::string& address, const std::string& name,
                                      bool hasLow, double low, bool hasHigh, double high,
                                      bool hasContactTimings, unsigned long initialAlertMinutes,
                                      unsigned long reminderAlertMinutes, bool hasSensorRefreshInterval,
                                      unsigned long sensorRefreshIntervalMinutes, std::string* error,
                                      bool hasContactTriggerSettings, bool contactTriggerMode,
                                      unsigned long contactTriggerCooldownSeconds) {
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
    command.hasContactTriggerSettings = hasContactTriggerSettings;
    command.contactTriggerMode = contactTriggerMode;
    command.contactTriggerCooldownSeconds = contactTriggerCooldownSeconds;
    return invoke(&command, error);
}
bool GatewayRunner::setDeviceValueAlerts(const std::string& address,
                                         const std::map<std::string, DeviceValueAlert>& alerts,
                                         std::string* error) {
    Command command;
    command.type = DeviceValueAlerts;
    command.address = address;
    command.valueAlerts = alerts;
    return invoke(&command, error);
}
bool GatewayRunner::deleteDevice(const std::string& address, bool local, std::string* error) {
    Command command; command.type = DeviceDelete; command.address = address; command.enabled = local;
    return invoke(&command, error);
}
bool GatewayRunner::deviceDeletionStatus(const std::string& address, std::string* json, std::string* error) {
    Command command; command.type = DeviceDeleteStatus; command.address = address;
    bool result = invoke(&command, error); *json = command.configuration; return result;
}
bool GatewayRunner::matterDeletionTarget(const std::string& address, std::string* node, std::string* error) {
    Command command; command.type = MatterDeleteTarget; command.address = address;
    bool result = invoke(&command, error); *node = command.name; return result;
}
bool GatewayRunner::removeMatterNode(const std::string& node, std::string* error) {
    Command command; command.type = MatterNodeRemove; command.address = node; return invoke(&command, error);
}
bool GatewayRunner::reconcileMatterNodes(const std::vector<std::string>& nodes, std::string* error) {
    Command command; command.type = MatterNodesReconcile; command.ids = nodes; return invoke(&command, error);
}
bool GatewayRunner::refreshDevice(const std::string& address, std::string* error) {
    Command command;
    command.type = DeviceRefresh;
    command.address = address;
    return invoke(&command, error);
}
bool GatewayRunner::reconfigureDevice(const std::string& address, std::string* error) {
    Command command;
    command.type = DeviceReconfigure;
    command.address = address;
    return invoke(&command, error);
}
bool GatewayRunner::identifyDevice(const std::string& address, std::string* error) {
    Command command;
    command.type = DeviceIdentify;
    command.address = address;
    return invoke(&command, error);
}
bool GatewayRunner::queryDeviceConfiguration(const std::string& address, std::string* error) {
    Command command;
    command.type = DeviceConfiguration;
    command.address = address;
    return invoke(&command, error);
}
bool GatewayRunner::setThermostat(const std::string& address, const std::string& field,
                                  const std::string& value, std::string* error, std::string* matterRequest) {
    Command command; command.type = ThermostatSetting; command.address = address;
    command.name = field; command.configuration = value;
    bool result=invoke(&command,error); if(result && matterRequest)*matterRequest=command.configuration; return result;
}
bool GatewayRunner::thermostatResult(const std::string& node, unsigned short endpoint, const std::string& field, const std::string& error) {
    Command command; command.type=ThermostatResult; command.matterReading.node=node; command.matterReading.endpoint=endpoint;
    command.name=field; command.configuration=error; std::string ignored; return invoke(&command,&ignored);
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
bool GatewayRunner::setBroadcastQuorum(unsigned int value, std::string* error) {
    Command command; command.type=GatewaySettings; command.seconds=value; return invoke(&command,error);
}
bool GatewayRunner::setMatterCommissioningRegion(const std::string& countryCode, const std::string& location,
                                                  std::string* error) {
    Command command; command.type=MatterRegionSettings; command.address=countryCode; command.name=location;
    return invoke(&command,error);
}
bool GatewayRunner::setEventLoopDebug(bool enabled, std::string* error) {
    Command command;
    command.type = EventLoopDebug;
    command.enabled = enabled;
    return invoke(&command, error);
}

bool GatewayRunner::exportNetworkBackup(const std::string& id, std::string* error) {
    Command command; command.type = NetworkBackup; command.address = id; return invoke(&command, error);
}
