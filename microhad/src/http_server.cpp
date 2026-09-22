#include "http_server.h"
#include "gateway_runner.h"
#include "storage.h"
#include "json_util.h"
#include "web_push.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
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
    response << "HTTP/1.1 " << status << (status == 200 ? " OK" : status == 500 ? " Internal Server Error" : " Bad Request") << "\r\n"
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

HttpServer::HttpServer(int port, GatewayRunner* gateway, const std::string& dashboardPath,
                       const std::string& certificatePath, const std::string& privateKeyPath) :
    port_(port), listenFd_(-1), gateway_(gateway), webPush_(new WebPushStore(gateway->runtimeDirectory())), dashboardPath_(dashboardPath),
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
    } else if (request.find("GET /api/scenes/config ") == 0) {
        std::ifstream input((gateway_->runtimeDirectory() + "/scenes.conf").c_str());
        std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        writeResponse(clientFd, 200, "text/plain; charset=utf-8", content);
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
    } else if (request.find("POST /api/scenes/config ") == 0) {
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string error;
        if (bodyAt == std::string::npos) writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"missing scene configuration\"}");
        else if (gateway_->setSceneConfiguration(request.substr(bodyAt + 4), &error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
    } else if (request.find("POST /api/scenes/") == 0) {
        const std::string prefix = "POST /api/scenes/";
        std::string tail = request.substr(prefix.size(), request.find(' ', prefix.size()) - prefix.size());
        std::vector<std::string> parts; std::string::size_type start = 0;
        while (start <= tail.size()) { std::string::size_type slash = tail.find('/', start); parts.push_back(tail.substr(start, slash == std::string::npos ? std::string::npos : slash - start)); if (slash == std::string::npos) break; start = slash + 1; }
        unsigned long group = 0, scene = 0; std::string error;
        if (parts.empty() || !parseMinutes(parts[0], &group) || group == 0 || group > 0xfff7)
            writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid scene group\"}");
        else if (parts.size() == 2 && parts[1] == "off") {
            if (gateway_->turnOffSceneGroup(static_cast<unsigned short>(group), &error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
            else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
        } else if (parts.size() == 3 && parts[2] == "activate" && parseMinutes(parts[1], &scene) && scene > 0 && scene < 256) {
            if (gateway_->activateScene(static_cast<unsigned short>(group), static_cast<unsigned char>(scene), &error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
            else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
        } else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid scene action\"}");
    } else if (request.find("POST /api/devices/") == 0) {
        const std::string prefix = "POST /api/devices/";
        std::string::size_type settingsAt = request.find("/settings ", prefix.size());
        std::string::size_type refreshAt = request.find("/refresh ", prefix.size());
        std::string::size_type lightAt = request.find("/light ", prefix.size());
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        if (lightAt != std::string::npos) {
            std::string ieeeAddress = decodeComponent(request.substr(prefix.size(), lightAt - prefix.size()));
            std::string state;
            std::string brightnessValue;
            std::string error;
            unsigned long brightness = 0;
            bool hasState = bodyAt != std::string::npos && formValue(request.substr(bodyAt + 4), "state", &state);
            bool hasBrightness = bodyAt != std::string::npos && formValue(request.substr(bodyAt + 4), "brightness", &brightnessValue);
            if ((!hasState && !hasBrightness) || (hasBrightness && (!parseMinutes(brightnessValue, &brightness) || brightness > 100))) {
                writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid light command\"}");
            } else if (gateway_->setLightState(ieeeAddress, hasState ? state : "", hasBrightness ? static_cast<int>(brightness) : -1, &error)) {
                writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
            } else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
        } else if (refreshAt != std::string::npos) {
            std::string ieeeAddress = decodeComponent(request.substr(prefix.size(), refreshAt - prefix.size()));
            std::string error;
            if (gateway_->refreshDevice(ieeeAddress, &error))
                writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"message\":\"Attribute refresh queued. Battery sensors reply during their next wake window.\"}");
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
            std::string sensorRefreshValue;
            std::string error;
            bool hasName = formValue(body, "name", &name);
            formValue(body, "low", &lowValue);
            formValue(body, "high", &highValue);
            bool hasInitial = formValue(body, "initial_minutes", &initialValue);
            bool hasReminder = formValue(body, "reminder_minutes", &reminderValue);
            bool hasSensorRefresh = formValue(body, "sensor_refresh_minutes", &sensorRefreshValue);
            bool hasLow = !lowValue.empty();
            bool hasHigh = !highValue.empty();
            double low = 0;
            double high = 0;
            unsigned long initial = 0;
            unsigned long reminder = 0;
            unsigned long sensorRefresh = 0;
            if (!hasName || (hasLow && !parseDecimal(lowValue, &low)) || (hasHigh && !parseDecimal(highValue, &high)) ||
                (hasInitial && !parseMinutes(initialValue, &initial)) ||
                (hasReminder && !parseMinutes(reminderValue, &reminder)) ||
                (hasSensorRefresh && !parseMinutes(sensorRefreshValue, &sensorRefresh))) {
                writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid device settings\"}");
            } else if (gateway_->setDeviceSettings(ieeeAddress, name, hasLow, low, hasHigh, high,
                                                    hasInitial && hasReminder, initial, reminder, hasSensorRefresh,
                                                    sensorRefresh, &error)) {
                writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
            } else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
        }
    } else if (request.find("GET / ") == 0) {
        std::string dashboard;
        if (readTextFile(dashboardPath_, &dashboard) && !dashboard.empty())
            writeResponse(clientFd, 200, "text/html; charset=utf-8", dashboard);
        else
            writeResponse(clientFd, 500, "text/plain; charset=utf-8", "Dashboard is not installed.");
    } else {
        writeResponse(clientFd, 400, "application/json", "{\"error\":\"unknown endpoint\"}");
    }
}
