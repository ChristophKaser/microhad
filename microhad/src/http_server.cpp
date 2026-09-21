#include "http_server.h"
#include "gateway_runner.h"
#include "storage.h"
#include "web_push.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <ctime>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#ifdef MICROHAD_MIPS_TARGET
extern "C" int microhad_tls_server_create(const char* certificatePath, const char* privateKeyPath, void** server);
extern "C" void microhad_tls_server_destroy(void* server);
extern "C" int microhad_tls_session_open(void* server, int fd, void** session);
extern "C" int microhad_tls_read_request(void* session, char* request, unsigned long capacity);
extern "C" int microhad_tls_write(void* session, const char* response, unsigned long length);
extern "C" void microhad_tls_session_close(void* session);
#endif

namespace {
std::string* queuedResponse = 0;
pthread_mutex_t webPushMutex = PTHREAD_MUTEX_INITIALIZER;
#ifdef MICROHAD_MIPS_TARGET
pthread_key_t tlsSessionKey;
pthread_once_t tlsSessionKeyOnce = PTHREAD_ONCE_INIT;
void createTlsSessionKey() { pthread_key_create(&tlsSessionKey, 0); }
void* activeTlsSession() { pthread_once(&tlsSessionKeyOnce, createTlsSessionKey); return pthread_getspecific(tlsSessionKey); }
void setActiveTlsSession(void* value) { pthread_once(&tlsSessionKeyOnce, createTlsSessionKey); pthread_setspecific(tlsSessionKey, value); }
#endif

void writeResponse(int fd, int status, const char* contentType, const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << (status == 200 ? " OK" : " Bad Request") << "\r\n"
             << "Content-Type: " << contentType << "\r\nCache-Control: no-store\r\nConnection: close\r\n"
             << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::string wire = response.str();
    const char* cursor = wire.data();
    unsigned long remaining = wire.size();
    if (queuedResponse != 0) {
        *queuedResponse = wire;
        return;
    }
#ifdef MICROHAD_MIPS_TARGET
    if (activeTlsSession() != 0) {
        microhad_tls_write(activeTlsSession(), wire.data(), wire.size());
        return;
    }
#endif
    while (remaining != 0) {
        ssize_t written = write(fd, cursor, remaining);
        if (written <= 0) break;
        cursor += written;
        remaining -= static_cast<unsigned long>(written);
    }
}

std::string decodeComponent(const std::string& value) {
    std::string decoded;
    unsigned long i;
    for (i = 0; i < value.size(); ++i) {
        if (value[i] == '+' ) decoded += ' ';
        else if (value[i] == '%' && i + 2 < value.size()) {
            char hex[3];
            hex[0] = value[i + 1]; hex[1] = value[i + 2]; hex[2] = 0;
            decoded += static_cast<char>(strtoul(hex, 0, 16));
            i += 2;
        } else decoded += value[i];
    }
    return decoded;
}

bool formValue(const std::string& form, const std::string& key, std::string* value) {
    std::string::size_type start = 0;
    while (start <= form.size()) {
        std::string::size_type end = form.find('&', start);
        std::string part = form.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.compare(0, key.size(), key) == 0 && part.size() > key.size() && part[key.size()] == '=') {
            *value = decodeComponent(part.substr(key.size() + 1));
            return true;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

bool parseDecimal(const std::string& value, double* result) {
    char* end = 0;
    errno = 0;
    *result = strtod(value.c_str(), &end);
    return errno == 0 && end != value.c_str() && *end == 0 && *result == *result;
}

bool parseMinutes(const std::string& value, unsigned long* result) {
    char* end = 0;
    errno = 0;
    if (value.empty() || value[0] == '-') return false;
    *result = strtoul(value.c_str(), &end, 10);
    return errno == 0 && end != value.c_str() && *end == 0;
}
}

HttpServer::HttpServer(int port, GatewayRunner* gateway, const std::string& certificatePath, const std::string& privateKeyPath) :
    port_(port), listenFd_(-1), gateway_(gateway), webPush_(new WebPushStore(gateway->runtimeDirectory())),
    certificatePath_(certificatePath), privateKeyPath_(privateKeyPath), tlsServer_(0), running_(false) {
    pthread_mutex_init(&queueMutex_, 0);
    pthread_cond_init(&queueReady_, 0);
}

HttpServer::~HttpServer() {
    pthread_mutex_lock(&queueMutex_);
    if (running_) {
        running_ = false;
        if (listenFd_ >= 0) { close(listenFd_); listenFd_ = -1; }
        pthread_cond_broadcast(&queueReady_);
        pthread_mutex_unlock(&queueMutex_);
        pthread_join(acceptThread_, 0);
        pthread_join(workerThreads_[0], 0);
        pthread_join(workerThreads_[1], 0);
    } else pthread_mutex_unlock(&queueMutex_);
    while (!clientQueue_.empty()) { close(clientQueue_.back()); clientQueue_.pop_back(); }
    pthread_cond_destroy(&queueReady_);
    pthread_mutex_destroy(&queueMutex_);
#ifdef MICROHAD_MIPS_TARGET
    if (tlsServer_ != 0) microhad_tls_server_destroy(tlsServer_);
#endif
    delete webPush_;
}

bool HttpServer::start(std::string* error) {
    struct sockaddr_in address;
    int reuse = 1;
#ifdef MICROHAD_MIPS_TARGET
    if (!certificatePath_.empty() || !privateKeyPath_.empty()) {
        if (certificatePath_.empty() || privateKeyPath_.empty() ||
            microhad_tls_server_create(certificatePath_.c_str(), privateKeyPath_.c_str(), &tlsServer_) != 0) {
            *error = "cannot initialize TLS certificate or key";
            return false;
        }
    }
#endif
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) { *error = strerror(errno); return false; }
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<unsigned short>(port_));
    if (bind(listenFd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0 || listen(listenFd_, 8) != 0) {
        *error = strerror(errno); close(listenFd_); listenFd_ = -1; return false;
    }
    fcntl(listenFd_, F_SETFL, O_NONBLOCK);
    pthread_mutex_lock(&queueMutex_);
    running_ = true;
    pthread_mutex_unlock(&queueMutex_);
    if (pthread_create(&acceptThread_, 0, acceptThreadMain, this) != 0 ||
        pthread_create(&workerThreads_[0], 0, workerThreadMain, this) != 0 ||
        pthread_create(&workerThreads_[1], 0, workerThreadMain, this) != 0) {
        *error = "cannot create HTTP worker threads";
        pthread_mutex_lock(&queueMutex_); running_ = false; close(listenFd_); listenFd_ = -1;
        pthread_cond_broadcast(&queueReady_); pthread_mutex_unlock(&queueMutex_);
        return false;
    }
    return true;
}

void HttpServer::poll() {
    /* The acceptor and worker pool own web I/O. Retained for main-loop API compatibility. */
}

void* HttpServer::acceptThreadMain(void* opaque) { static_cast<HttpServer*>(opaque)->acceptLoop(); return 0; }
void* HttpServer::workerThreadMain(void* opaque) { static_cast<HttpServer*>(opaque)->workerLoop(); return 0; }

void HttpServer::acceptLoop() {
    int clientFd;
    while (true) {
        clientFd = accept(listenFd_, 0, 0);
        if (clientFd < 0) { usleep(10000); pthread_mutex_lock(&queueMutex_); bool keepGoing = running_; pthread_mutex_unlock(&queueMutex_); if (!keepGoing) break; continue; }
        pthread_mutex_lock(&queueMutex_);
        if (!running_) { pthread_mutex_unlock(&queueMutex_); close(clientFd); break; }
        if (clientQueue_.size() >= 16) { pthread_mutex_unlock(&queueMutex_); close(clientFd); continue; }
        clientQueue_.push_back(clientFd);
        pthread_cond_signal(&queueReady_);
        pthread_mutex_unlock(&queueMutex_);
    }
}

void HttpServer::workerLoop() {
    while (true) {
        int clientFd;
        pthread_mutex_lock(&queueMutex_);
        while (running_ && clientQueue_.empty()) pthread_cond_wait(&queueReady_, &queueMutex_);
        if (!running_ && clientQueue_.empty()) { pthread_mutex_unlock(&queueMutex_); break; }
        clientFd = clientQueue_.front(); clientQueue_.erase(clientQueue_.begin());
        pthread_mutex_unlock(&queueMutex_);
        struct timeval timeout;
        timeout.tv_sec = 5; timeout.tv_usec = 0;
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        handleClient(clientFd);
        close(clientFd);
    }
}

void HttpServer::handleClient(int clientFd) {
    char buffer[2048];
    ssize_t count;
    std::string request;
#ifdef MICROHAD_MIPS_TARGET
    void* session = 0;
    if (tlsServer_ != 0 && microhad_tls_session_open(tlsServer_, clientFd, &session) != 0) return;
#else
#endif
#ifdef MICROHAD_MIPS_TARGET
    if (session != 0) {
        if (microhad_tls_read_request(session, buffer, sizeof(buffer)) != 0) {
            microhad_tls_session_close(session);
            return;
        }
        count = static_cast<ssize_t>(strlen(buffer));
        setActiveTlsSession(session);
    } else count = read(clientFd, buffer, sizeof(buffer) - 1);
#else
    count = read(clientFd, buffer, sizeof(buffer) - 1);
#endif
    if (count > 0) {
        buffer[count] = 0;
        request.assign(buffer);
        handleRequest(clientFd, request);
    }
#ifdef MICROHAD_MIPS_TARGET
    setActiveTlsSession(0);
    if (session != 0) microhad_tls_session_close(session);
#endif
}

void HttpServer::handleRequest(int clientFd, const std::string& request) {
    if (request.find("GET /api/status ") == 0) {
        writeResponse(clientFd, 200, "application/json", gateway_->statusJson());
    } else if (request.find("POST /api/service/restart ") == 0) {
        writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"state\":\"restarting\"}");
        kill(getpid(), SIGTERM);
    } else if (request.find("GET /api/logs ") == 0) {
        writeResponse(clientFd, 200, "application/json", gateway_->logsJson());
    } else if (request.find("POST /api/join ") == 0) {
        std::string error;
        if (gateway_->setJoining(300, &error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"seconds\":300,\"state\":\"preparing\"}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + error + "\"}");
    } else if (request.find("POST /api/notifications/test ") == 0) {
        std::string error;
        if (gateway_->sendTestNotification(&error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
    } else if (request.find("POST /api/notifications/enabled ") == 0) {
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string error;
        bool enabled = bodyAt != std::string::npos && request.substr(bodyAt + 4).find("enabled=true") != std::string::npos;
        if (bodyAt == std::string::npos || (request.substr(bodyAt + 4).find("enabled=true") == std::string::npos &&
                                           request.substr(bodyAt + 4).find("enabled=false") == std::string::npos)) {
            writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid notification setting\"}");
        } else if (gateway_->setNotificationsEnabled(enabled, &error)) {
            writeResponse(clientFd, 200, "application/json", std::string("{\"ok\":true,\"enabled\":") + (enabled ? "true}" : "false}"));
        } else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
    } else if (request.find("GET /api/push/vapid-key ") == 0) {
        writeResponse(clientFd, 200, "application/json", "{\"public_key\":\"" + webPush_->publicKey() + "\"}");
    } else if (request.find("POST /api/push/subscriptions ") == 0) {
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string error;
        pthread_mutex_lock(&webPushMutex);
        bool subscribed = bodyAt != std::string::npos && webPush_->addSubscription(request.substr(bodyAt + 4), &error);
        pthread_mutex_unlock(&webPushMutex);
        if (subscribed)
            writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + error + "\"}");
    } else if (request.find("GET /sw.js ") == 0) {
        const char* worker = "self.addEventListener('push',function(e){var d={title:'MicroHAD',body:''};if(e.data){d=e.data.json()}e.waitUntil(self.registration.showNotification(d.title,{body:d.body}))});";
        writeResponse(clientFd, 200, "application/javascript", worker);
    } else if (request.find("POST /api/network/create ") == 0) {
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string confirmation;
        std::string error;
        if (bodyAt == std::string::npos || !formValue(request.substr(bodyAt + 4), "confirmation", &confirmation) ||
            confirmation != "DELETE") {
            writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"type DELETE to create a fresh network\"}");
        } else if (gateway_->createFreshNetwork(&error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"state\":\"forming\"}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + error + "\"}");
    } else if (request.find("POST /api/devices/") == 0) {
        const std::string prefix = "POST /api/devices/";
        std::string::size_type settingsAt = request.find("/settings ", prefix.size());
        std::string::size_type refreshAt = request.find("/refresh ", prefix.size());
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        if (refreshAt != std::string::npos) {
            std::string ieeeAddress = decodeComponent(request.substr(prefix.size(), refreshAt - prefix.size()));
            std::string error;
            if (gateway_->refreshDevice(ieeeAddress, &error))
                writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"message\":\"Refresh request sent. Battery sensors reply when they wake up.\"}");
            else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
        } else if (bodyAt == std::string::npos || settingsAt == std::string::npos) {
            writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid device settings request\"}");
        } else {
            std::string ieeeAddress = decodeComponent(request.substr(prefix.size(), settingsAt - prefix.size()));
            std::string body = request.substr(bodyAt + 4);
            std::string name;
            std::string lowValue;
            std::string highValue;
            std::string initialValue;
            std::string reminderValue;
            std::string error;
            bool hasName = formValue(body, "name", &name);
            formValue(body, "low", &lowValue);
            formValue(body, "high", &highValue);
            bool hasInitial = formValue(body, "initial_minutes", &initialValue);
            bool hasReminder = formValue(body, "reminder_minutes", &reminderValue);
            bool hasLow = !lowValue.empty();
            bool hasHigh = !highValue.empty();
            double low = 0;
            double high = 0;
            unsigned long initial = 0;
            unsigned long reminder = 0;
            if (!hasName || (hasLow && !parseDecimal(lowValue, &low)) || (hasHigh && !parseDecimal(highValue, &high)) ||
                (hasInitial && !parseMinutes(initialValue, &initial)) ||
                (hasReminder && !parseMinutes(reminderValue, &reminder))) {
                writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid device settings\"}");
            } else if (gateway_->setDeviceSettings(ieeeAddress, name, hasLow, low, hasHigh, high,
                                                    hasInitial && hasReminder, initial, reminder, &error)) {
                writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
            } else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
        }
    } else if (request.find("GET / ") == 0) {
#if 0
        const char* page = "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>MicroHAD</title><style>body{font:16px sans-serif;max-width:48rem;margin:auto;padding:1rem}button{padding:.7rem;margin:.2rem}.device{border-bottom:1px solid #ddd;padding:.6rem}</style><h1>MicroHAD</h1><p><button onclick=\"post('/api/network/create')\">Create fresh network</button><button id=join onclick=\"post('/api/join')\">Allow joining (5 min)</button><button onclick=\"enablePush()\">Enable browser alerts</button><button id=test-alerts onclick=\"post('/api/notifications/test')\">Test alerts</button><label><input id=notifications type=checkbox onchange=\"setNotifications(this.checked)\"> Push notifications</label></p><p id=state>Loading…</p><p id=join-state></p><div id=devices></div><script>function post(u,b){return fetch(u,{method:'POST',body:b||''}).then(function(r){return r.json()}).then(function(v){if(!v.ok)throw Error(v.error||'Request failed');return update()}).catch(function(e){alert(e.message)})}function setNotifications(v){post('/api/notifications/enabled','enabled='+(v?'true':'false'))}function b64(s){s+=(s.length%4?'='.repeat(4-s.length%4):'');return Uint8Array.from(atob(s.replace(/-/g,'+').replace(/_/g,'/')),function(c){return c.charCodeAt(0)})}function enablePush(){if(!('serviceWorker'in navigator)||!('PushManager'in window)){alert('Push is unavailable in this browser');return}navigator.serviceWorker.register('/sw.js').then(function(r){return fetch('/api/push/vapid-key').then(function(x){return x.json()}).then(function(k){return r.pushManager.subscribe({userVisibleOnly:true,applicationServerKey:b64(k.public_key)})})}).then(function(s){return fetch('/api/push/subscriptions',{method:'POST',body:JSON.stringify(s)})}).then(function(r){return r.json()}).then(function(v){if(!v.ok)throw Error(v.error||'Subscription failed');alert('Browser alerts enabled')}).catch(function(e){alert('Could not enable alerts: '+e.message)})}function rename(a,n){var v=prompt('Device name',n);if(v)post('/api/devices/'+encodeURIComponent(a)+'/name',v)}function temperatures(a,l,h){l=prompt('Low alert °C (blank disables)',l===null?'':l);if(l===null)return;h=prompt('High alert °C (blank disables)',h===null?'':h);if(h===null)return;post('/api/devices/'+encodeURIComponent(a)+'/temperature-alert','low='+encodeURIComponent(l)+'&high='+encodeURIComponent(h))}function update(){return fetch('/api/status').then(function(r){return r.json()}).then(function(v){document.getElementById('state').textContent='Coordinator: '+(v.online?'online':'offline')+' | Network: '+(v.network_formed?'formed':v.network_forming?'forming':'not formed');document.getElementById('notifications').checked=v.notifications_enabled;document.getElementById('test-alerts').disabled=!v.notifications_enabled;var left=Math.max(0,v.joining_until-Math.floor(Date.now()/1000));document.getElementById('join-state').textContent=left?'Joining enabled: pair a sensor within '+Math.ceil(left/60)+' minute(s).':'Joining is closed.';document.getElementById('join').textContent=left?'Extend joining (5 min)':'Allow joining (5 min)';document.getElementById('devices').innerHTML=v.devices.map(function(d){var s=d.ieee_address+' · '+d.kind;if(d.has_contact)s+=' · '+(d.contact_open?'OPEN':'closed');if(d.has_temperature)s+=' · '+d.temperature_celsius+' °C';if(d.has_battery)s+=' · battery '+d.battery_percent+'%';var b='<button onclick=\"rename(\\\''+d.ieee_address+'\\\',\\\''+d.name.replace(/'/g,'')+'\\\')\">Rename</button>';if(d.has_temperature)b+=' <button onclick=\"temperatures(\\\''+d.ieee_address+'\\\','+(d.has_low_temperature_alert?d.low_temperature_alert:'null')+','+(d.has_high_temperature_alert?d.high_temperature_alert:'null')+')\">Temperature alerts</button>';return '<div class=device><b>'+d.name+'</b> '+b+'<br>'+s+'</div>'}).join('')})}update();setInterval(update,2000)</script>";
#endif
        const char* page =
            "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>MicroHAD</title>"
            "<style>body{font:16px sans-serif;max-width:48rem;margin:auto;padding:1rem}button{padding:.7rem;margin:.2rem}.device{border-bottom:1px solid #ddd;padding:.6rem}.modal{position:fixed;inset:0;background:#0008;display:grid;place-items:center;padding:1rem}.modal[hidden]{display:none}.dialog{background:#fff;padding:1rem;max-width:28rem;width:100%;box-sizing:border-box}.dialog label{display:block;margin:.7rem 0}.dialog input{display:block;width:100%;box-sizing:border-box;padding:.4rem}.error{color:#b00;min-height:1.2em}</style>"
            "<h1>MicroHAD</h1><p><button onclick=\"post('/api/network/create')\">Create fresh network</button><button id=join onclick=\"post('/api/join')\">Allow joining (5 min)</button><button onclick=\"restartService()\">Restart service</button><button onclick=\"enablePush()\">Enable browser alerts</button><button id=test-alerts onclick=\"post('/api/notifications/test')\">Test alerts</button><label><input id=notifications type=checkbox onchange=\"setNotifications(this.checked)\"> Push notifications</label></p><p id=state>Loading…</p><p id=join-state></p><div id=devices></div>"
            "<div id=modal class=modal hidden><form id=settings-form class=dialog><h2>Device settings</h2><p id=settings-error class=error></p><label>Name<input id=settings-name maxlength=64 required></label><div id=contact-settings><label>First alert after (minutes)<input id=initial-minutes type=number min=1 step=1></label><label>Reminder every (minutes)<input id=reminder-minutes type=number min=1 step=1></label></div><div id=temperature-settings><label>Low threshold °C (blank disables)<input id=low-threshold type=number step=.1></label><label>High threshold °C (blank disables)<input id=high-threshold type=number step=.1></label></div><p><button type=button onclick=\"closeSettings()\">Cancel</button><button type=submit>Save</button></p></form></div>"
            "<script>var currentDevices=[],selectedDevice;function post(u,b){return fetch(u,{method:'POST',body:b||''}).then(function(r){return r.json()}).then(function(v){if(!v.ok)throw Error(v.error||'Request failed');return update()}).catch(function(e){alert(e.message)})}function setNotifications(v){post('/api/notifications/enabled','enabled='+(v?'true':'false'))}function b64(s){s+=(s.length%4?'='.repeat(4-s.length%4):'');return Uint8Array.from(atob(s.replace(/-/g,'+').replace(/_/g,'/')),function(c){return c.charCodeAt(0)})}function enablePush(){if(!('serviceWorker'in navigator)||!('PushManager'in window)){alert('Push is unavailable in this browser');return}navigator.serviceWorker.register('/sw.js').then(function(r){return fetch('/api/push/vapid-key').then(function(x){return x.json()}).then(function(k){return r.pushManager.subscribe({userVisibleOnly:true,applicationServerKey:b64(k.public_key)})})}).then(function(s){return fetch('/api/push/subscriptions',{method:'POST',body:JSON.stringify(s)})}).then(function(r){return r.json()}).then(function(v){if(!v.ok)throw Error(v.error||'Subscription failed');alert('Browser alerts enabled')}).catch(function(e){alert('Could not enable alerts: '+e.message)})}function esc(s){return String(s).replace(/[&<>]/g,function(c){return c=='&'?'&amp;':c=='<'?'&lt;':'&gt;'})}function settings(i){selectedDevice=currentDevices[i];var d=selectedDevice;document.getElementById('settings-name').value=d.name;document.getElementById('contact-settings').hidden=!d.has_contact;document.getElementById('temperature-settings').hidden=!d.has_temperature;document.getElementById('initial-minutes').required=!!d.has_contact;document.getElementById('reminder-minutes').required=!!d.has_contact;document.getElementById('initial-minutes').value=d.contact_initial_alert_minutes;document.getElementById('reminder-minutes').value=d.contact_reminder_alert_minutes;document.getElementById('low-threshold').value=d.has_low_temperature_alert?d.low_temperature_alert:'';document.getElementById('high-threshold').value=d.has_high_temperature_alert?d.high_temperature_alert:'';document.getElementById('settings-error').textContent='';document.getElementById('modal').hidden=false}function closeSettings(){document.getElementById('modal').hidden=true}document.getElementById('settings-form').onsubmit=function(e){e.preventDefault();var d=selectedDevice,parts=['name='+encodeURIComponent(document.getElementById('settings-name').value)];if(d.has_temperature){parts.push('low='+encodeURIComponent(document.getElementById('low-threshold').value));parts.push('high='+encodeURIComponent(document.getElementById('high-threshold').value))}if(d.has_contact){parts.push('initial_minutes='+encodeURIComponent(document.getElementById('initial-minutes').value));parts.push('reminder_minutes='+encodeURIComponent(document.getElementById('reminder-minutes').value))}fetch('/api/devices/'+encodeURIComponent(d.ieee_address)+'/settings',{method:'POST',body:parts.join('&')}).then(function(r){return r.json()}).then(function(v){if(!v.ok)throw Error(v.error||'Request failed');closeSettings();return update()}).catch(function(x){document.getElementById('settings-error').textContent=x.message})};function update(){return fetch('/api/status').then(function(r){return r.json()}).then(function(v){currentDevices=v.devices;document.getElementById('state').textContent='Coordinator: '+(v.online?'online':'offline')+' | Network: '+(v.network_formed?'formed':v.network_forming?'forming':'not formed');document.getElementById('notifications').checked=v.notifications_enabled;document.getElementById('test-alerts').disabled=!v.notifications_enabled;var left=Math.max(0,v.joining_until-Math.floor(Date.now()/1000));document.getElementById('join-state').textContent=left?'Joining enabled: pair a sensor within '+Math.ceil(left/60)+' minute(s).':'Joining is closed.';document.getElementById('join').textContent=left?'Extend joining (5 min)':'Allow joining (5 min)';document.getElementById('devices').innerHTML=v.devices.map(function(d,i){var s=d.ieee_address+' · '+d.kind;if(d.has_contact)s+=' · '+(d.contact_open?'OPEN':'closed');if(d.has_temperature)s+=' · '+d.temperature_celsius+' °C';if(d.has_battery)s+=' · battery '+d.battery_percent+'%';return '<div class=device><b>'+esc(d.name)+'</b> <button onclick=\"settings('+i+')\">Settings</button><br>'+esc(s)+'</div>'}).join('')})}update();setInterval(update,2000)</script>";
        std::string dashboard(page);
        std::string settingsButton = "<button onclick=\"settings('+i+')\">Settings</button>";
        std::string refreshButton = settingsButton + " <button onclick=\"refreshDevice('+i+')\">Refresh</button>";
        std::string::size_type settingsButtonAt = dashboard.find(settingsButton);
        if (settingsButtonAt != std::string::npos) dashboard.replace(settingsButtonAt, settingsButton.size(), refreshButton);
        std::string refreshMarker = "function update(){";
        std::string::size_type refreshMarkerAt = dashboard.find(refreshMarker);
        if (refreshMarkerAt != std::string::npos) {
            dashboard.insert(refreshMarkerAt,
                             "function refreshDevice(i){var d=currentDevices[i];fetch('/api/devices/'+encodeURIComponent(d.ieee_address)+'/refresh',{method:'POST'}).then(function(r){return r.json()}).then(function(v){if(!v.ok)throw Error(v.error||'Refresh failed');alert(v.message||'Refresh request sent');return update()}).catch(function(e){alert(e.message)})}");
        }
        std::string createButton = "<button onclick=\"post('/api/network/create')\">Create fresh network</button>";
        std::string confirmedCreateButton = "<button onclick=\"openNetworkConfirmation()\">Create fresh network</button>";
        std::string networkDialog =
            "<div id=network-modal class=modal hidden><form id=network-confirmation-form class=dialog>"
            "<h2>Create a fresh Zigbee network</h2><p>This deletes the current Zigbee network. All paired devices "
            "must be joined again.</p><p>Type <b>DELETE</b> to continue.</p><p id=network-confirmation-error class=error></p>"
            "<label>Confirmation<input id=network-confirmation autocomplete=off oninput=\"updateNetworkConfirmation()\"></label>"
            "<p><button type=button onclick=\"closeNetworkConfirmation()\">Cancel</button>"
            "<button id=network-confirmation-submit type=submit disabled>Create fresh network</button></p></form></div>";
        std::string networkScript =
            "function openNetworkConfirmation(){document.getElementById('network-confirmation').value='';"
            "document.getElementById('network-confirmation-error').textContent='';updateNetworkConfirmation();"
            "document.getElementById('network-modal').hidden=false}function closeNetworkConfirmation(){"
            "document.getElementById('network-modal').hidden=true}function updateNetworkConfirmation(){"
            "document.getElementById('network-confirmation-submit').disabled=document.getElementById('network-confirmation').value!=='DELETE'}"
            "document.getElementById('network-confirmation-form').onsubmit=function(e){e.preventDefault();"
            "var c=document.getElementById('network-confirmation').value;if(c!=='DELETE')return;"
            "fetch('/api/network/create',{method:'POST',body:'confirmation='+encodeURIComponent(c)}).then(function(r){return r.json()})"
            ".then(function(v){if(!v.ok)throw Error(v.error||'Request failed');closeNetworkConfirmation();return update()})"
            ".catch(function(x){document.getElementById('network-confirmation-error').textContent=x.message})};";
        std::string::size_type confirmationAt = dashboard.find(createButton);
        if (confirmationAt != std::string::npos)
            dashboard.replace(confirmationAt, createButton.size(), confirmedCreateButton);
        confirmationAt = dashboard.find("<div id=devices></div>");
        if (confirmationAt != std::string::npos)
            dashboard.insert(confirmationAt + 22, networkDialog);
        confirmationAt = dashboard.find("<script>");
        if (confirmationAt != std::string::npos)
            dashboard.insert(confirmationAt + 8, networkScript);
        std::string marker = "if(d.has_battery)";
        std::string::size_type markerAt = dashboard.find(marker);
        if (markerAt != std::string::npos) {
            dashboard.insert(markerAt, "if(d.has_humidity)s+=' · '+d.humidity_percent+'% humidity';if(d.has_occupancy)s+=' · '+(d.occupied?'OCCUPIED':'clear');");
        }
        std::string oldJoinState = "var left=Math.max(0,v.joining_until-Math.floor(Date.now()/1000));document.getElementById('join-state').textContent=left?'Joining enabled: pair a sensor within '+Math.ceil(left/60)+' minute(s).':'Joining is closed.';document.getElementById('join').textContent=left?'Extend joining (5 min)':'Allow joining (5 min)';";
        std::string newJoinState = "var left=Math.max(0,v.joining_until-Math.floor(Date.now()/1000)),preparing=v.joining_preparing;document.getElementById('join-state').textContent=left?'Joining enabled: pair a sensor within '+Math.ceil(left/60)+' minute(s).':preparing?'Preparing secure pairing…':'Joining is closed.';document.getElementById('join').textContent=left?'Extend joining (5 min)':preparing?'Preparing…':'Allow joining (5 min)';";
        markerAt = dashboard.find(oldJoinState);
        if (markerAt != std::string::npos) dashboard.replace(markerAt, oldJoinState.size(), newJoinState);
        std::string restartScript = ";function restartService(){if(!confirm('Restart the Zigbee service? Joining will close briefly.'))return;fetch('/api/service/restart',{method:'POST'}).then(function(r){return r.json()}).then(function(v){if(!v.ok)throw Error(v.error||'Restart failed');document.getElementById('state').textContent='Restarting service…';setTimeout(update,3000)}).catch(function(e){alert(e.message)})}";
        markerAt = dashboard.find("</script>");
        if (markerAt != std::string::npos) dashboard.insert(markerAt, restartScript);
        std::string oldCoordinatorState = "' | Network: '+(v.network_formed?'formed':v.network_forming?'forming':'not formed')";
        std::string newCoordinatorState = "' | Network: '+(v.network_formed?'formed':v.network_forming?'forming':'not formed')+' | Serial RX: '+v.serial_rx_bytes+' bytes / '+v.ash_rx_frames+' frames'";
        markerAt = dashboard.find(oldCoordinatorState);
        if (markerAt != std::string::npos) {
            dashboard.replace(markerAt, oldCoordinatorState.size(), newCoordinatorState);
        }
        markerAt = dashboard.find("<div id=devices></div>");
        if (markerAt != std::string::npos) {
            dashboard.replace(markerAt, strlen("<div id=devices></div>"),
                              "<div id=devices></div><details open><summary>Live coordinator log</summary><pre id=logs style='max-height:16rem;overflow:auto;white-space:pre-wrap'></pre></details>");
        }
        markerAt = dashboard.find("</script>");
        if (markerAt != std::string::npos) {
            dashboard.insert(markerAt, ";function updateLogs(){fetch('/api/logs').then(function(r){return r.json()}).then(function(v){document.getElementById('logs').textContent=v.lines.join('\\n')})}updateLogs();setInterval(updateLogs,1000);");
        }
        writeResponse(clientFd, 200, "text/html; charset=utf-8", dashboard);
    } else {
        writeResponse(clientFd, 400, "application/json", "{\"error\":\"unknown endpoint\"}");
    }
}
