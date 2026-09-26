#include "http_server.h"
#include "gateway_runner.h"
#include "storage.h"
#include "json_util.h"
#include "web_push.h"
#include "sensor_history.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <ctime>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <vector>

#ifdef MICROHAD_MBEDTLS_ENABLED
extern "C" int microhad_tls_server_create(const char* certificatePath, const char* privateKeyPath, void** server);
extern "C" void microhad_tls_server_destroy(void* server);
extern "C" int microhad_tls_session_open(void* server, int fd, void** session);
extern "C" int microhad_tls_read_request(void* session, char* request, unsigned long capacity);
extern "C" int microhad_tls_write(void* session, const char* response, unsigned long length);
extern "C" void microhad_tls_session_close(void* session);
#endif

namespace {
const unsigned long maximumRequestBytes = 16384;

enum RequestReadResult {
    RequestReadOk,
    RequestReadMalformed,
    RequestReadTooLarge,
    RequestReadFailed
};

std::string* queuedResponse = 0;
pthread_mutex_t webPushMutex = PTHREAD_MUTEX_INITIALIZER;
#ifdef MICROHAD_MBEDTLS_ENABLED
pthread_key_t tlsSessionKey;
pthread_once_t tlsSessionKeyOnce = PTHREAD_ONCE_INIT;
void createTlsSessionKey() { pthread_key_create(&tlsSessionKey, 0); }
void* activeTlsSession() { pthread_once(&tlsSessionKeyOnce, createTlsSessionKey); return pthread_getspecific(tlsSessionKey); }
void setActiveTlsSession(void* value) { pthread_once(&tlsSessionKeyOnce, createTlsSessionKey); pthread_setspecific(tlsSessionKey, value); }
#endif

void writeResponse(int fd, int status, const char* contentType, const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << (status == 200 ? " OK" : status == 500 ? " Internal Server Error" :
                                           status == 413 ? " Payload Too Large" : status == 404 ? " Not Found" : " Bad Request") << "\r\n"
             << "Content-Type: " << contentType << "\r\nCache-Control: no-store\r\nConnection: close\r\n"
             << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::string wire = response.str();
    const char* cursor = wire.data();
    unsigned long remaining = wire.size();
    if (queuedResponse != 0) {
        *queuedResponse = wire;
        return;
    }
#ifdef MICROHAD_MBEDTLS_ENABLED
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

bool equalHeaderName(const std::string& name, const char* expected) {
    unsigned long i;
    if (name.size() != strlen(expected)) return false;
    for (i = 0; i < name.size(); ++i) {
        char value = name[i];
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value + ('a' - 'A'));
        if (value != expected[i]) return false;
    }
    return true;
}

RequestReadResult expectedRequestLength(const std::string& request, unsigned long* expected) {
    std::string::size_type headerEnd = request.find("\r\n\r\n");
    std::string::size_type lineStart;
    bool sawContentLength = false;
    unsigned long contentLength = 0;
    if (headerEnd == std::string::npos) return RequestReadFailed;
    if (headerEnd + 4 > maximumRequestBytes) return RequestReadTooLarge;
    lineStart = request.find("\r\n");
    if (lineStart == std::string::npos || lineStart == 0) return RequestReadMalformed;
    lineStart += 2;
    while (lineStart < headerEnd) {
        std::string::size_type lineEnd = request.find("\r\n", lineStart);
        std::string::size_type colon;
        std::string name;
        std::string value;
        if (lineEnd == std::string::npos || lineEnd > headerEnd) return RequestReadMalformed;
        colon = request.find(':', lineStart);
        if (colon == std::string::npos || colon >= lineEnd) return RequestReadMalformed;
        name = request.substr(lineStart, colon - lineStart);
        value = request.substr(colon + 1, lineEnd - colon - 1);
        while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value.erase(0, 1);
        if (equalHeaderName(name, "transfer-encoding")) return RequestReadMalformed;
        if (equalHeaderName(name, "content-length")) {
            char* end = 0;
            unsigned long parsed;
            if (value.empty() || value[0] == '-') return RequestReadMalformed;
            errno = 0;
            parsed = strtoul(value.c_str(), &end, 10);
            if (errno != 0 || end == value.c_str() || *end != 0 || sawContentLength)
                return RequestReadMalformed;
            sawContentLength = true;
            contentLength = parsed;
        }
        lineStart = lineEnd + 2;
    }
    if (contentLength > maximumRequestBytes - static_cast<unsigned long>(headerEnd + 4)) return RequestReadTooLarge;
    *expected = static_cast<unsigned long>(headerEnd + 4) + contentLength;
    return RequestReadOk;
}

RequestReadResult readPlainRequest(int fd, std::string* request) {
    char chunk[1024];
    unsigned long expected = 0;
    bool haveHeaders = false;
    request->clear();
    while (request->size() < maximumRequestBytes) {
        ssize_t received = read(fd, chunk, sizeof(chunk));
        if (received <= 0) return RequestReadFailed;
        request->append(chunk, static_cast<unsigned long>(received));
        if (!haveHeaders && request->find("\r\n\r\n") != std::string::npos) {
            RequestReadResult result = expectedRequestLength(*request, &expected);
            if (result != RequestReadOk) return result;
            haveHeaders = true;
        }
        if (haveHeaders && request->size() >= expected) {
            request->resize(expected);
            return RequestReadOk;
        }
    }
    return RequestReadTooLarge;
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

const char* assetContentType(const std::string& path) {
    std::string::size_type dot = path.rfind('.');
    if (dot == std::string::npos) return 0;
    const std::string extension = path.substr(dot);
    if (extension == ".html") return "text/html; charset=utf-8";
    if (extension == ".js") return "application/javascript; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".ico") return "image/x-icon";
    if (extension == ".woff2") return "font/woff2";
    if (extension == ".json") return "application/json";
    return 0;
}

bool safeAssetPath(const std::string& path) {
    if (path.empty() || path[0] != '/' || path.find('%') != std::string::npos ||
        path.find('\\') != std::string::npos || path.find("//") != std::string::npos) return false;
    std::string::size_type start = 1;
    while (start < path.size()) {
        std::string::size_type end = path.find('/', start);
        std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == ".." || part[0] == '.') return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

}

HttpServer::HttpServer(int port, GatewayRunner* gateway, const std::string& webuiDirectory,
                       const std::string& certificatePath, const std::string& privateKeyPath) :
    port_(port), listenFd_(-1), gateway_(gateway), webPush_(new WebPushStore(gateway->runtimeDirectory())), webuiDirectory_(webuiDirectory),
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
#ifdef MICROHAD_MBEDTLS_ENABLED
    if (tlsServer_ != 0) microhad_tls_server_destroy(tlsServer_);
#endif
    delete webPush_;
}

bool HttpServer::start(std::string* error) {
    struct sockaddr_in address;
    int reuse = 1;
#ifdef MICROHAD_MBEDTLS_ENABLED
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

int HttpServer::boundPort() const {
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    if (listenFd_ < 0 || getsockname(listenFd_, reinterpret_cast<struct sockaddr*>(&address), &length) != 0) return -1;
    return static_cast<int>(ntohs(address.sin_port));
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
    std::string request;
    RequestReadResult readResult = RequestReadFailed;
#ifdef MICROHAD_MBEDTLS_ENABLED
    void* session = 0;
    if (tlsServer_ != 0 && microhad_tls_session_open(tlsServer_, clientFd, &session) != 0) return;
#else
#endif
#ifdef MICROHAD_MBEDTLS_ENABLED
    if (session != 0) {
        std::vector<char> buffer(maximumRequestBytes + 1, 0);
        int tlsResult = microhad_tls_read_request(session, &buffer[0], buffer.size());
        if (tlsResult != 0) {
            setActiveTlsSession(session);
            writeResponse(clientFd, tlsResult == -2 ? 413 : 400, "application/json",
                          tlsResult == -2 ? "{\"error\":\"request is too large\"}" : "{\"error\":\"malformed request\"}");
            setActiveTlsSession(0);
            microhad_tls_session_close(session);
            return;
        }
        request.assign(&buffer[0]);
        setActiveTlsSession(session);
    } else readResult = readPlainRequest(clientFd, &request);
#else
    readResult = readPlainRequest(clientFd, &request);
#endif
    if (
#ifdef MICROHAD_MBEDTLS_ENABLED
        session != 0 ||
#endif
        readResult == RequestReadOk) {
        handleRequest(clientFd, request);
    } else {
        writeResponse(clientFd, readResult == RequestReadTooLarge ? 413 : 400, "application/json",
                      readResult == RequestReadTooLarge ? "{\"error\":\"request is too large\"}" : "{\"error\":\"malformed request\"}");
    }
#ifdef MICROHAD_MBEDTLS_ENABLED
    setActiveTlsSession(0);
    if (session != 0) microhad_tls_session_close(session);
#endif
}

void HttpServer::handleRequest(int clientFd, const std::string& request) {
    if (request.find("GET /api/status ") == 0) {
        GatewayRunner::ViewLease* view = gateway_->acquireView();
        if (view == 0) writeResponse(clientFd, 503, "application/json", "{\"ok\":false,\"error\":\"coordinator is unavailable\"}");
        else { std::string body = view->statusJson(); delete view; writeResponse(clientFd, 200, "application/json", body); }
    } else if (request.find("GET /api/devices/") == 0) {
        const std::string prefix = "GET /api/devices/";
        std::string::size_type end = request.find(' ', prefix.size());
        std::string target = end == std::string::npos ? "" : request.substr(prefix.size(), end - prefix.size());
        std::string::size_type suffix = target.find("/history");
        bool valid = suffix != std::string::npos && suffix > 0;
        std::string address = valid ? decodeComponent(target.substr(0, suffix)) : "";
        std::string query = valid ? target.substr(suffix + 8) : "";
        unsigned long before = 0;
        valid = valid && (query.empty() || query.find("?before=") == 0);
        if (valid && !query.empty()) {
            const std::string number = query.substr(8);
            char* parseEnd = 0;
            errno = 0;
            before = strtoul(number.c_str(), &parseEnd, 10);
            valid = !number.empty() && errno == 0 && *parseEnd == 0 && before > 0 && before <= 0xffffffffUL;
        }
        std::string json;
        std::string error;
        if (!valid || !SensorHistory::readJson(gateway_->historyPath(), address, before, &json, &error))
            writeResponse(clientFd, valid && error != "invalid device address" ? 500 : 400, "application/json",
                          "{\"ok\":false,\"error\":\"" + jsonEscape(valid ? error : "invalid history request") + "\"}");
        else writeResponse(clientFd, 200, "application/json", json);
    } else if (request.find("GET /api/scenes/config ") == 0) {
        std::ifstream input((gateway_->runtimeDirectory() + "/scenes.conf").c_str());
        std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        writeResponse(clientFd, 200, "text/plain; charset=utf-8", content);
    } else if (request.find("POST /api/service/restart ") == 0) {
        writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"state\":\"restarting\"}");
        kill(getpid(), SIGTERM);
    } else if (request.find("GET /api/logs ") == 0) {
        GatewayRunner::ViewLease* view = gateway_->acquireView();
        if (view == 0) writeResponse(clientFd, 503, "application/json", "{\"ok\":false,\"error\":\"coordinator is unavailable\"}");
        else { std::string body = view->logsJson(); delete view; writeResponse(clientFd, 200, "application/json", body); }
    } else if (request.find("POST /api/debug/event-loop ") == 0) {
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string enabledValue;
        std::string error;
        bool hasEnabled = bodyAt != std::string::npos && formValue(request.substr(bodyAt + 4), "enabled", &enabledValue);
        bool enabled = enabledValue == "true";
        if (!hasEnabled || (enabledValue != "true" && enabledValue != "false")) {
            writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid event-loop debug setting\"}");
        } else if (gateway_->setEventLoopDebug(enabled, &error)) {
            writeResponse(clientFd, 200, "application/json", std::string("{\"ok\":true,\"enabled\":") + (enabled ? "true}" : "false}"));
        } else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
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
        std::string enabledValue;
        std::string error;
        bool hasEnabled = bodyAt != std::string::npos && formValue(request.substr(bodyAt + 4), "enabled", &enabledValue);
        bool enabled = enabledValue == "true";
        if (!hasEnabled || (enabledValue != "true" && enabledValue != "false")) {
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
        std::string::size_type identifyAt = request.find("/identify ", prefix.size());
        std::string::size_type configurationAt = request.find("/configuration ", prefix.size());
        std::string::size_type lightAt = request.find("/light ", prefix.size());
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        if (configurationAt != std::string::npos) {
            std::string ieeeAddress = decodeComponent(request.substr(prefix.size(), configurationAt - prefix.size()));
            std::string error;
            if (gateway_->queryDeviceConfiguration(ieeeAddress, &error))
                writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"message\":\"Configuration query sent. A sleepy device may reply when it wakes.\"}");
            else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
        } else if (lightAt != std::string::npos) {
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
        } else if (identifyAt != std::string::npos) {
            std::string ieeeAddress = decodeComponent(request.substr(prefix.size(), identifyAt - prefix.size()));
            std::string error;
            if (gateway_->identifyDevice(ieeeAddress, &error))
                writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"message\":\"Identify command queued for five seconds. A sleepy device may respond later.\"}");
            else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
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
    } else if (request.find("GET /") == 0 && request.find("GET /api/") != 0) {
        std::string::size_type end = request.find(' ', 4);
        std::string path = request.substr(4, end - 4);
        std::string::size_type query = path.find('?');
        if (query != std::string::npos) path.erase(query);
        if (path == "/") path = "/index.html";
        const char* contentType = assetContentType(path);
        if (!safeAssetPath(path) || contentType == 0) {
            writeResponse(clientFd, 404, "text/plain; charset=utf-8", "Asset not found.");
        } else {
            std::string filename = webuiDirectory_ + path;
            struct stat info;
            std::string body;
            if (lstat(filename.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
                readTextFile(filename, &body))
                writeResponse(clientFd, 200, contentType, body);
            else if (path == "/index.html")
                writeResponse(clientFd, 500, "text/plain; charset=utf-8", "Dashboard is not installed.");
            else writeResponse(clientFd, 404, "text/plain; charset=utf-8", "Asset not found.");
        }
    } else {
        writeResponse(clientFd, 400, "application/json", "{\"error\":\"unknown endpoint\"}");
    }
}
