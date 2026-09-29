#include "external_http_device.h"
#include "device_fritz_box.h"
#include "device_shelly.h"
#include "gateway_runner.h"

#include "json_util.h"

#include "mbedtls/md5.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <map>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace {
bool validHost(const std::string& host) {
    unsigned long i;
    if (host.empty() || host.size() > 200 || host.find('/') != std::string::npos ||
        host.find('@') != std::string::npos || host.find(' ') != std::string::npos) return false;
    for (i = 0; i < host.size(); ++i) {
        char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == ':')) return false;
    }
    return true;
}

bool validId(const std::string& id) {
    if (id.size() != 16) return false;
    for (unsigned long i = 0; i < id.size(); ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return true;
}

std::string base64(const std::string& input) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    unsigned long i;
    for (i = 0; i < input.size(); i += 3) {
        unsigned long value = static_cast<unsigned char>(input[i]) << 16;
        if (i + 1 < input.size()) value |= static_cast<unsigned char>(input[i + 1]) << 8;
        if (i + 2 < input.size()) value |= static_cast<unsigned char>(input[i + 2]);
        result += alphabet[(value >> 18) & 63];
        result += alphabet[(value >> 12) & 63];
        result += i + 1 < input.size() ? alphabet[(value >> 6) & 63] : '=';
        result += i + 2 < input.size() ? alphabet[value & 63] : '=';
    }
    return result;
}

std::string md5Hex(const std::string& value) {
    unsigned char digest[16];
    if (mbedtls_md5_ret(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest) != 0) return "";
    static const char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned long i = 0; i < sizeof(digest); ++i) {
        result.push_back(hex[digest[i] >> 4]); result.push_back(hex[digest[i] & 15]);
    }
    return result;
}

std::string digestParameter(const std::string& challenge, const std::string& name) {
    std::string key = name + "=\"";
    std::string::size_type at = challenge.find(key);
    if (at == std::string::npos) return "";
    at += key.size();
    std::string::size_type end = challenge.find('"', at);
    return end == std::string::npos ? "" : challenge.substr(at, end - at);
}

std::string digestAuthorization(const std::string& challenge, const std::string& username,
                                const std::string& password, const std::string& method,
                                const std::string& uri) {
    std::string realm = digestParameter(challenge, "realm");
    std::string nonce = digestParameter(challenge, "nonce");
    std::string qop = digestParameter(challenge, "qop");
    if (realm.empty() || nonce.empty()) return "";
    bool useQop = qop.find("auth") != std::string::npos;
    struct timeval now; gettimeofday(&now, 0);
    std::ostringstream cnonceStream; cnonceStream << std::hex << now.tv_sec << now.tv_usec;
    std::string cnonce = cnonceStream.str();
    std::string ha1 = md5Hex(username + ":" + realm + ":" + password);
    std::string ha2 = md5Hex(method + ":" + uri);
    std::string response = useQop ? md5Hex(ha1 + ":" + nonce + ":00000001:" + cnonce + ":auth:" + ha2)
                                  : md5Hex(ha1 + ":" + nonce + ":" + ha2);
    if (response.empty()) return "";
    std::ostringstream result;
    result << "Authorization: Digest username=\"" << username << "\", realm=\"" << realm
           << "\", nonce=\"" << nonce << "\", uri=\"" << uri << "\", response=\"" << response << "\"";
    if (useQop) result << ", qop=auth, nc=00000001, cnonce=\"" << cnonce << "\"";
    result << "\r\n";
    return result.str();
}

bool httpRequest(const std::string& configuredHost, const std::string& username, const std::string& password,
                 const std::string& method, const std::string& path, const std::string& headers,
                 const std::string& requestBody, std::string* body, unsigned short defaultPort = 80,
                 const std::string& authorization = "", bool digestOnly = false,
                 bool acceptSoapFault = false) {
    std::string host = configuredHost;
    unsigned short port = defaultPort;
    std::string::size_type colon = host.rfind(':');
    if (colon != std::string::npos && host.find(']') == std::string::npos) {
        char* end = 0;
        long parsed = strtol(host.c_str() + colon + 1, &end, 10);
        if (end != host.c_str() + colon + 1 && *end == 0 && parsed > 0 && parsed <= 65535) {
            port = static_cast<unsigned short>(parsed);
            host.erase(colon);
        }
    }
    struct hostent* resolved = gethostbyname(host.c_str());
    if (resolved == 0 || resolved->h_addr_list == 0 || resolved->h_addr_list[0] == 0) return false;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct timeval timeout;
    timeout.tv_sec = 3; timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    memcpy(&address.sin_addr, resolved->h_addr_list[0], sizeof(address.sin_addr));
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) { close(fd); return false; }
    int connected = connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address));
    if (connected != 0 && errno == EINPROGRESS) {
        fd_set writable;
        FD_ZERO(&writable); FD_SET(fd, &writable);
        struct timeval connectTimeout;
        connectTimeout.tv_sec = 3; connectTimeout.tv_usec = 0;
        if (select(fd + 1, 0, &writable, 0, &connectTimeout) <= 0) { close(fd); return false; }
        int socketError = 0;
        socklen_t socketErrorLength = sizeof(socketError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorLength) != 0 || socketError != 0) {
            close(fd); return false;
        }
    } else if (connected != 0) { close(fd); return false; }
    if (fcntl(fd, F_SETFL, flags) != 0) { close(fd); return false; }
    std::ostringstream request;
    request << method << " " << path << " HTTP/1.1\r\nHost: " << configuredHost << "\r\nConnection: close\r\n";
    if (!authorization.empty()) request << authorization;
    else if (!username.empty() && !digestOnly) request << "Authorization: Basic " << base64(username + ":" + password) << "\r\n";
    request << headers;
    if (!requestBody.empty()) request << "Content-Length: " << requestBody.size() << "\r\n";
    request << "\r\n" << requestBody;
    std::string wire = request.str();
    unsigned long sent = 0;
    while (sent < wire.size()) {
        ssize_t n = write(fd, wire.data() + sent, wire.size() - sent);
        if (n <= 0) { close(fd); return false; }
        sent += static_cast<unsigned long>(n);
    }
    std::string response;
    char buffer[2048];
    while (response.size() < 16384) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n <= 0) break;
        response.append(buffer, static_cast<unsigned long>(n));
    }
    close(fd);
    std::string::size_type lineEnd = response.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    std::string::size_type headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return false;
    bool unauthorized = response.compare(0, 12, "HTTP/1.1 401") == 0 || response.compare(0, 12, "HTTP/1.0 401") == 0;
    if (unauthorized && !username.empty() && authorization.empty()) {
        std::string::size_type challengeAt = response.find("WWW-Authenticate:");
        if (challengeAt == std::string::npos) challengeAt = response.find("www-authenticate:");
        if (challengeAt != std::string::npos) {
            std::string::size_type challengeEnd = response.find("\r\n", challengeAt);
            std::string challenge = response.substr(challengeAt, challengeEnd - challengeAt);
            if (challenge.find("Digest ") != std::string::npos) {
                std::string auth = digestAuthorization(challenge, username, password, method, path);
                if (!auth.empty()) return httpRequest(configuredHost, username, password, method, path, headers,
                                                     requestBody, body, defaultPort, auth, digestOnly, acceptSoapFault);
            }
        }
    }
    bool ok = response.compare(0, 12, "HTTP/1.1 200") == 0 || response.compare(0, 12, "HTTP/1.0 200") == 0;
    bool soapFault = response.compare(0, 12, "HTTP/1.1 500") == 0 || response.compare(0, 12, "HTTP/1.0 500") == 0;
    if (!ok && !(acceptSoapFault && soapFault)) return false;
    *body = response.substr(headerEnd + 4);
    return true;
}

bool httpGet(const ShellyDevice& device, const std::string& path, std::string* body) {
    return httpRequest(device.host, device.username, device.password, "GET", path, "", "", body);
}

bool jsonBoolField(const std::string& object, const char* name, bool* value) {
    std::string key = std::string("\"") + name + "\"";
    std::string::size_type at = object.find(key);
    if (at == std::string::npos) return false;
    at = object.find(':', at + key.size());
    if (at == std::string::npos) return false;
    ++at;
    while (at < object.size() && (object[at] == ' ' || object[at] == '\t')) ++at;
    if (object.compare(at, 4, "true") == 0) { *value = true; return true; }
    if (object.compare(at, 5, "false") == 0) { *value = false; return true; }
    return false;
}

std::string objectAt(const std::string& input, const std::string& key, int index) {
    std::string::size_type at = input.find(std::string("\"") + key + "\"");
    if (at == std::string::npos) return "";
    at = input.find('[', at);
    if (at == std::string::npos) return "";
    ++at;
    int current = 0;
    while (at < input.size()) {
        at = input.find('{', at);
        if (at == std::string::npos) return "";
        std::string::size_type start = at;
        int depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (; at < input.size(); ++at) {
            char c = input[at];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) {
                if (current == index) return input.substr(start, at - start + 1);
                ++current; ++at; break;
            }
        }
    }
    return "";
}

bool extractIndexedNumber(const std::string& input, const char* parent, int index, const char* field, double* value) {
    std::ostringstream number;
    number << index;
    std::string::size_type at = input.find(std::string("\"") + parent + "\"");
    if (at == std::string::npos) return false;
    at = input.find(std::string("\"") + number.str() + "\"", at);
    if (at == std::string::npos) return false;
    at = input.find('{', at);
    if (at == std::string::npos) return false;
    std::string object = input.substr(at, input.find('}', at) - at + 1);
    std::string key = std::string("\"") + field + "\"";
    at = object.find(key);
    if (at == std::string::npos) return false;
    at = object.find(':', at + key.size());
    if (at == std::string::npos) return false;
    char* end = 0;
    *value = strtod(object.c_str() + at + 1, &end);
    return end != object.c_str() + at + 1 && *value == *value;
}

bool randomToken(std::string* token) {
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    unsigned long got = 0;
    while (got < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + got, sizeof(bytes) - got);
        if (n <= 0) { close(fd); return false; }
        got += static_cast<unsigned long>(n);
    }
    close(fd);
    static const char hex[] = "0123456789abcdef";
    token->clear();
    for (got = 0; got < sizeof(bytes); ++got) { token->push_back(hex[bytes[got] >> 4]); token->push_back(hex[bytes[got] & 15]); }
    return true;
}

std::string callbackUrl(const ShellyDevice& device, const std::string& event) {
    return std::string("http://") + device.callbackHost + "/api/shelly/callback/" + device.id + "/" + event + "?token=" + device.callbackToken;
}

bool parseIndices(const std::string& text, int maximum, bool allowEmpty, std::vector<int>* values) {
    values->clear();
    if (text.empty()) return allowEmpty;
    std::string::size_type start = 0;
    while (start < text.size()) {
        std::string::size_type end = text.find(',', start);
        std::string part = text.substr(start, end == std::string::npos ? text.size() - start : end - start);
        if (part.size() != 1 || part[0] < '0' || part[0] > '9') return false;
        int index = part[0] - '0';
        if (index > maximum || std::find(values->begin(), values->end(), index) != values->end()) return false;
        values->push_back(index);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return !values->empty();
}

std::string indicesText(const std::vector<int>& values) {
    std::ostringstream out;
    for (unsigned long i = 0; i < values.size(); ++i) { if (i) out << ','; out << values[i]; }
    return out.str();
}

int firstIndex(const std::vector<int>& values, int fallback) { return values.empty() ? fallback : values[0]; }
std::string lowerAscii(std::string value) {
    for (unsigned long i = 0; i < value.size(); ++i) if (value[i] >= 'A' && value[i] <= 'Z') value[i] = static_cast<char>(value[i] - 'A' + 'a');
    return value;
}
bool validIndices(const std::vector<int>& values, int maximum) {
    for (unsigned long i = 0; i < values.size(); ++i)
        if (values[i] < 0 || values[i] > maximum || std::find(values.begin(), values.begin() + i, values[i]) != values.begin() + i) return false;
    return true;
}

std::string deviceJson(const ShellyDevice& d, bool includeCallbackUrls) {
    std::ostringstream out;
    out << "{\"id\":\"" << jsonEscape(d.id) << "\",\"name\":\"" << jsonEscape(d.name)
        << "\",\"host\":\"" << jsonEscape(d.host) << "\",\"callback_host\":\"" << jsonEscape(d.callbackHost)
        << "\",\"model\":\"" << jsonEscape(d.model)
        << "\",\"http_auth\":" << (!d.username.empty() ? "true" : "false")
        << ",\"input_index\":" << firstIndex(d.inputIndices, -1)
        << ",\"temperature_index\":" << firstIndex(d.temperatureIndices, -1)
        << ",\"relay_index\":" << firstIndex(d.relayIndices, 0)
        << ",\"input_indices\":\"" << indicesText(d.inputIndices)
        << "\",\"temperature_indices\":\"" << indicesText(d.temperatureIndices)
        << "\",\"relay_indices\":\"" << indicesText(d.relayIndices) << "\",\"poll_seconds\":" << d.pollSeconds
        << ",\"invert_contact\":" << (d.invertContact ? "true" : "false")
        << ",\"callback_mode\":" << (d.callbackToken.empty() ? "false" : "true");
    if (includeCallbackUrls && !d.callbackToken.empty() && !d.callbackHost.empty()) {
        out << ",\"callback_urls\":{";
        bool first = true;
        for (unsigned long i = 0; i < d.inputIndices.size(); ++i) {
            int channel = d.inputIndices[i]; std::ostringstream suffix; suffix << (channel + 1);
            std::string prefix = channel == 0 ? "input" : "input_" + suffix.str();
            if (!first) out << ',';
            first = false;
            out << "\"" << prefix << "_open\":\"" << jsonEscape(callbackUrl(d, channel == 0 ? "input-on" : "input-on-" + suffix.str()))
                << "\",\"" << prefix << "_closed\":\"" << jsonEscape(callbackUrl(d, channel == 0 ? "input-off" : "input-off-" + suffix.str())) << "\"";
        }
        std::vector<int> callbackRelays = d.relayIndices;
        if (d.model != "uni" && callbackRelays.empty()) callbackRelays.push_back(0);
        for (unsigned long i = 0; i < callbackRelays.size(); ++i) {
            int channel = callbackRelays[i]; std::ostringstream suffix; suffix << (channel + 1);
            std::string prefix = channel == 0 ? "relay" : "relay_" + suffix.str();
            if (!first) out << ',';
            first = false;
            out << "\"" << prefix << "_on\":\"" << jsonEscape(callbackUrl(d, channel == 0 ? "relay-on" : "relay-on-" + suffix.str()))
                << "\",\"" << prefix << "_off\":\"" << jsonEscape(callbackUrl(d, channel == 0 ? "relay-off" : "relay-off-" + suffix.str())) << "\"";
        }
        out << '}';
    }
    out << "}";
    return out.str();
}

}

bool externalHttpRequest(const std::string& host, const std::string& username, const std::string& password,
                        const std::string& method, const std::string& path, const std::string& headers,
                        const std::string& requestBody, std::string* responseBody, unsigned short defaultPort,
                        bool digestOnly, bool acceptSoapFault) {
    if (!validHost(host) || (method != "GET" && method != "POST") || path.empty() || path[0] != '/' ||
        path.find('\r') != std::string::npos || path.find('\n') != std::string::npos || path.find(' ') != std::string::npos ||
        username.find('\r') != std::string::npos || username.find('\n') != std::string::npos ||
        password.find('\r') != std::string::npos || password.find('\n') != std::string::npos) return false;
    return httpRequest(host, username, password, method, path, headers, requestBody, responseBody, defaultPort, "", digestOnly, acceptSoapFault);
}

ShellyDevice::ShellyDevice() : pollSeconds(30), invertContact(false), nextPoll(0), manualRefresh(false) {}
FritzBoxClient::FritzBoxClient() : connected(false), hasSignal(false), signalPercent(0) {}
FritzBoxDevice::FritzBoxDevice() : pollSeconds(30), nextPoll(0) {}

ExternalHttpDeviceWorker::ExternalHttpDeviceWorker(const std::string& runtimeDirectory, GatewayRunner* gateway) : runtimeDirectory_(runtimeDirectory), gateway_(gateway), running_(false), started_(false) {
    pthread_mutex_init(&mutex_, 0);
    pthread_cond_init(&ready_, 0);
}

ExternalHttpDeviceWorker::~ExternalHttpDeviceWorker() {
    stop();
    pthread_cond_destroy(&ready_);
    pthread_mutex_destroy(&mutex_);
}

bool ExternalHttpDeviceWorker::start(std::string* error) {
    if (!load(error)) return false;
    if (!loadFritzBoxes(error)) return false;
    if (gateway_ != 0) {
        std::vector<std::string> ids;
        for (unsigned long i = 0; i < devices_.size(); ++i) ids.push_back(devices_[i].id);
        if (!gateway_->reconcileShelly(ids, error)) return false;
        ids.clear();
        for (unsigned long i = 0; i < fritzBoxes_.size(); ++i)
            for (unsigned long j = 0; j < fritzBoxes_[i].clients.size(); ++j) ids.push_back(fritzBoxes_[i].clients[j].id);
        if (!gateway_->reconcileFritzClients(ids, error)) return false;
    }
    for (unsigned long i = 0; gateway_ != 0 && i < devices_.size(); ++i)
        if (!gateway_->registerShelly(devices_[i].id, devices_[i].name, devices_[i].name,
                                      devices_[i].model, devices_[i].inputIndices, devices_[i].temperatureIndices,
                                      devices_[i].relayIndices, error)) return false;
    for (unsigned long i = 0; gateway_ != 0 && i < fritzBoxes_.size(); ++i) {
        for (unsigned long j = 0; j < fritzBoxes_[i].clients.size(); ++j) {
            const FritzBoxClient& client = fritzBoxes_[i].clients[j];
            if (!gateway_->registerShelly(client.id, client.name, client.name, "fritz", std::vector<int>(), std::vector<int>(), std::vector<int>(), error)) return false;
        }
    }
    pthread_mutex_lock(&mutex_);
    if (started_) { pthread_mutex_unlock(&mutex_); *error = "Shelly integration already started"; return false; }
    running_ = true;
    if (pthread_create(&thread_, 0, threadMain, this) != 0) {
        running_ = false; pthread_mutex_unlock(&mutex_); *error = "cannot create Shelly integration worker"; return false;
    }
    started_ = true;
    pthread_mutex_unlock(&mutex_);
    return true;
}

void ExternalHttpDeviceWorker::stop() {
    pthread_mutex_lock(&mutex_);
    if (!started_) { pthread_mutex_unlock(&mutex_); return; }
    running_ = false;
    for (unsigned long i = 0; i < discoveries_.size(); ++i) {
        discoveries_[i]->error = "external device worker is stopping";
        discoveries_[i]->done = true;
        pthread_cond_signal(&discoveries_[i]->ready);
    }
    discoveries_.clear();
    pthread_cond_broadcast(&ready_);
    pthread_mutex_unlock(&mutex_);
    pthread_join(thread_, 0);
    pthread_mutex_lock(&mutex_); started_ = false; pthread_mutex_unlock(&mutex_);
}

void* ExternalHttpDeviceWorker::threadMain(void* opaque) { static_cast<ExternalHttpDeviceWorker*>(opaque)->run(); return 0; }

void ExternalHttpDeviceWorker::run() {
    while (true) {
        pthread_mutex_lock(&mutex_);
        if (!running_) { pthread_mutex_unlock(&mutex_); break; }
        pthread_mutex_unlock(&mutex_);
        executeCallbacks();
        executeCommands();
        executeDiscoveries();
        pollDevices();
        pollFritzBoxes();
        pthread_mutex_lock(&mutex_);
        if (running_) {
            struct timespec deadline;
            deadline.tv_sec = time(0) + 1; deadline.tv_nsec = 0;
            pthread_cond_timedwait(&ready_, &mutex_, &deadline);
        }
        pthread_mutex_unlock(&mutex_);
    }
}

bool ExternalHttpDeviceWorker::parseFritzHostList(const std::string& xml, std::vector<FritzBoxClient>* clients) {
    return FritzBoxProtocol::parseHostList(xml, clients);
}

bool ExternalHttpDeviceWorker::discoverFritzClients(const std::string& host, const std::string& username, const std::string& password,
                                                    std::vector<FritzBoxClient>* clients, std::string* error) {
    DiscoveryRequest request;
    request.host = host; request.username = username; request.password = password;
    pthread_mutex_lock(&mutex_);
    if (!started_ || !running_) { pthread_mutex_unlock(&mutex_); *error = "external device worker is unavailable"; return false; }
    discoveries_.push_back(&request);
    pthread_cond_signal(&ready_);
    while (!request.done) pthread_cond_wait(&request.ready, &mutex_);
    bool result = request.result;
    *error = request.error;
    *clients = request.clients;
    pthread_mutex_unlock(&mutex_);
    return result;
}

void ExternalHttpDeviceWorker::executeDiscoveries() {
    DiscoveryRequest* request = 0;
    pthread_mutex_lock(&mutex_);
    if (!discoveries_.empty()) { request = discoveries_[0]; discoveries_.erase(discoveries_.begin()); }
    pthread_mutex_unlock(&mutex_);
    if (request == 0) return;
    std::vector<FritzBoxClient> clients;
    std::string error;
    bool result = discoverFritzClientsNetwork(request->host, request->username, request->password, &clients, &error);
    pthread_mutex_lock(&mutex_);
    request->clients = clients; request->error = error; request->result = result; request->done = true;
    pthread_cond_signal(&request->ready);
    pthread_mutex_unlock(&mutex_);
}

bool ExternalHttpDeviceWorker::discoverFritzClientsNetwork(const std::string& host, const std::string& username, const std::string& password,
                                                           std::vector<FritzBoxClient>* clients, std::string* error) {
    if (!validHost(host) || username.size() > 128 || password.size() > 256 ||
        username.find('\r') != std::string::npos || username.find('\n') != std::string::npos ||
        password.find('\r') != std::string::npos || password.find('\n') != std::string::npos) {
        *error = "invalid Fritz!Box host or credentials"; return false;
    }
    std::string effectiveUser = username, effectivePassword = password;
    if (effectivePassword.empty()) {
        pthread_mutex_lock(&mutex_);
        for (unsigned long i = 0; i < fritzBoxes_.size(); ++i) if (fritzBoxes_[i].host == host) {
            if (effectiveUser.empty()) effectiveUser = fritzBoxes_[i].username;
            effectivePassword = fritzBoxes_[i].password; break;
        }
        pthread_mutex_unlock(&mutex_);
    }
    const std::string& user = effectiveUser;
    const std::string& pass = effectivePassword;
    const std::string hostsService = "urn:dslforum-org:service:Hosts:1";
    const std::string wlanService = "urn:dslforum-org:service:WLANConfiguration:1";
    std::string reply, path;
    /* The AVM host-list download action requires an administrator role on
       newer FRITZ!OS releases. The standard Hosts:1 enumeration actions are
       readable by an ordinary home-network user and provide the same fields. */
    bool enumerated = false;
    if (FritzBoxProtocol::soapAction(host, user, pass, hostsService, "GetHostNumberOfEntries", "/upnp/control/hosts", "", &reply, error)) {
        std::string countText = FritzBoxProtocol::xmlValue(reply, "NewHostNumberOfEntries");
        char* end = 0;
        long count = strtol(countText.c_str(), &end, 10);
        if (end != countText.c_str() && *end == 0 && count >= 0 && count <= 512) {
            clients->clear(); enumerated = true;
            for (long index = 0; index < count; ++index) {
                std::ostringstream args; args << "<NewIndex>" << index << "</NewIndex>";
                std::string entry;
                if (!FritzBoxProtocol::soapAction(host, user, pass, hostsService, "GetGenericHostEntry", "/upnp/control/hosts", args.str(), &entry)) { enumerated = false; break; }
                std::string interfaceType = FritzBoxProtocol::xmlValue(entry, "NewInterfaceType");
                if (interfaceType != "802.11") continue;
                FritzBoxClient client;
                client.mac = FritzBoxProtocol::xmlValue(entry, "NewMACAddress");
                for (unsigned long j = 0; j < client.mac.size(); ++j) if (client.mac[j] >= 'A' && client.mac[j] <= 'F') client.mac[j] = static_cast<char>(client.mac[j] - 'A' + 'a');
                client.name = FritzBoxProtocol::xmlUnescape(FritzBoxProtocol::xmlValue(entry, "NewHostName"));
                client.connected = FritzBoxProtocol::xmlValue(entry, "NewActive") == "1";
                if (!client.mac.empty()) clients->push_back(client);
            }
        }
    }
    if (!enumerated) {
        if (!FritzBoxProtocol::soapAction(host, user, pass, hostsService, "X_AVM-DE_GetHostListPath", "/upnp/control/hosts", "", &reply, error)) {
            if (error->empty()) *error = "cannot query Fritz!Box host list; check TR-064 access and credentials";
            return false;
        }
        path = FritzBoxProtocol::xmlValue(reply, "NewX_AVM-DE_HostListPath");
        if (path.empty() || !externalHttpRequest(host, user, pass, "GET", path, "", "", &reply, 49000, true, false) || !parseFritzHostList(reply, clients)) {
            *error = "Fritz!Box returned an invalid host list"; return false;
        }
    }
    std::string wlanReply;
    if (FritzBoxProtocol::soapAction(host, user, pass, wlanService, "X_AVM-DE_GetWLANDeviceListPath", "/upnp/control/wlanconfig1", "", &wlanReply)) {
        path = FritzBoxProtocol::xmlValue(wlanReply, "NewX_AVM-DE_WLANDeviceListPath");
        std::string wlanXml;
        if (!path.empty() && externalHttpRequest(host, user, pass, "GET", path, "", "", &wlanXml, 49000, true, false))
            FritzBoxProtocol::mergeSignalList(wlanXml, clients);
    }
    return true;
}

void ExternalHttpDeviceWorker::executeCallbacks() {
    std::vector<CallbackEvent> events;
    pthread_mutex_lock(&mutex_); events.swap(callbacks_); pthread_mutex_unlock(&mutex_);
    if (gateway_ == 0) return;
    for (unsigned long i = 0; i < events.size(); ++i) {
        std::string error;
        gateway_->applyShellyEvent(events[i].id, events[i].metric, events[i].channelIndex, events[i].value, &error);
    }
}

void ExternalHttpDeviceWorker::executeCommands() {
    std::vector<ActorCommand> commands;
    pthread_mutex_lock(&mutex_); commands.swap(commands_); pthread_mutex_unlock(&mutex_);
    unsigned long i;
    for (i = 0; i < commands.size(); ++i) {
        ShellyDevice device;
        bool found = false;
        pthread_mutex_lock(&mutex_);
        for (unsigned long j = 0; j < devices_.size(); ++j) if (devices_[j].id == commands[i].id) { device = devices_[j]; found = true; break; }
        pthread_mutex_unlock(&mutex_);
        if (found) {
            bool configured = device.model != "uni" ||
                std::find(device.relayIndices.begin(), device.relayIndices.end(), commands[i].relayIndex) != device.relayIndices.end();
            if (!configured) continue;
            std::ostringstream path; path << "/relay/" << commands[i].relayIndex << "?turn=" << (commands[i].on ? "on" : "off");
            std::string response;
            if (httpGet(device, path.str(), &response)) {
                bool state = false;
                if (jsonBoolField(response, "ison", &state)) {
                    if (gateway_ != 0) { std::string error; gateway_->applyShellyEvent(device.id, 3, commands[i].relayIndex, state ? 1 : 0, &error); }
                }
            }
        }
    }
}

bool ExternalHttpDeviceWorker::parsePowerStatus(const std::string& body, double* watts) {
    return ShellyProtocol::parsePowerStatus(body, watts);
}

void ExternalHttpDeviceWorker::pollDevices() {
    std::vector<ShellyDevice> snapshot;
    long now = static_cast<long>(time(0));
    pthread_mutex_lock(&mutex_);
    for (unsigned long i = 0; i < devices_.size(); ++i) {
        if (devices_[i].nextPoll <= now) devices_[i].nextPoll = now + static_cast<long>(devices_[i].pollSeconds);
    }
    snapshot = devices_;
    for (unsigned long i = 0; i < devices_.size(); ++i) devices_[i].manualRefresh = false;
    pthread_mutex_unlock(&mutex_);
    for (unsigned long i = 0; i < snapshot.size(); ++i) {
        ShellyDevice& d = snapshot[i];
        if ((!d.manualRefresh && !d.callbackToken.empty() && d.model == "uni" && d.temperatureIndices.empty()) ||
            d.nextPoll != now + static_cast<long>(d.pollSeconds)) continue;
        std::string body;
        if (!httpGet(d, "/status", &body)) continue;
        if (d.model == "uni") {
            for (unsigned long j = 0; j < d.inputIndices.size(); ++j) {
                int channel = d.inputIndices[j]; std::string input = objectAt(body, "inputs", channel); long raw = -1;
                bool known = !input.empty() && jsonLongField(input, "input", &raw) && (raw == 0 || raw == 1);
                if (gateway_ != 0 && (d.callbackToken.empty() || d.manualRefresh) && known) {
                    std::string error; gateway_->applyShellyEvent(d.id, 1, channel, ((raw != 0) != d.invertContact) ? 1 : 0, &error);
                }
            }
            for (unsigned long j = 0; j < d.relayIndices.size(); ++j) {
                int channel = d.relayIndices[j]; std::string relay = objectAt(body, "relays", channel); bool state = false;
                bool known = !relay.empty() && jsonBoolField(relay, "ison", &state);
                if (gateway_ != 0 && (d.callbackToken.empty() || d.manualRefresh) && known) {
                    std::string error; gateway_->applyShellyEvent(d.id, 3, channel, state ? 1 : 0, &error);
                }
            }
            for (unsigned long j = 0; j < d.temperatureIndices.size(); ++j) {
                int channel = d.temperatureIndices[j]; double temperature = 0; std::string error;
                if (gateway_ != 0 && extractIndexedNumber(body, "ext_temperature", channel, "tC", &temperature))
                    gateway_->applyShellyEvent(d.id, 2, channel, temperature, &error);
            }
        } else {
            std::string relay = objectAt(body, "relays", 0); bool actorOn = false;
            bool hasActor = !relay.empty() && jsonBoolField(relay, "ison", &actorOn);
            if (gateway_ != 0 && hasActor && (d.callbackToken.empty() || d.manualRefresh)) {
                std::string error; gateway_->applyShellyEvent(d.id, 3, 0, actorOn ? 1 : 0, &error);
            }
        }
        if (gateway_ != 0 && d.model != "uni") {
            double watts = 0;
            std::string error;
            if (parsePowerStatus(body, &watts)) gateway_->applyShellyEvent(d.id, 4, 0, watts, &error);
        }
    }
}

void ExternalHttpDeviceWorker::pollFritzBoxes() {
    std::vector<FritzBoxDevice> snapshot;
    long now = static_cast<long>(time(0));
    pthread_mutex_lock(&mutex_);
    for (unsigned long i = 0; i < fritzBoxes_.size(); ++i)
        if (fritzBoxes_[i].nextPoll <= now) fritzBoxes_[i].nextPoll = now + fritzBoxes_[i].pollSeconds;
    snapshot = fritzBoxes_;
    pthread_mutex_unlock(&mutex_);
    for (unsigned long i = 0; i < snapshot.size(); ++i) {
        if (snapshot[i].nextPoll != now + static_cast<long>(snapshot[i].pollSeconds)) continue;
        std::vector<FritzBoxClient> clients;
        std::string error;
        if (!discoverFritzClientsNetwork(snapshot[i].host, snapshot[i].username, snapshot[i].password, &clients, &error)) continue;
        for (unsigned long j = 0; j < snapshot[i].clients.size(); ++j) {
            FritzBoxClient selected = snapshot[i].clients[j];
            selected.connected = false;
            selected.hasSignal = false;
            for (unsigned long k = 0; k < clients.size(); ++k) if (clients[k].mac == selected.mac) {
                selected.connected = clients[k].connected; selected.hasSignal = clients[k].hasSignal; selected.signalPercent = clients[k].signalPercent; break;
            }
            if (gateway_ != 0) gateway_->applyFritzEvent(selected.id, selected.connected, selected.hasSignal, selected.signalPercent, &error);
        }
    }
}

std::string ExternalHttpDeviceWorker::fritzJson() const {
    std::ostringstream out; pthread_mutex_lock(&mutex_); out << '[';
    for (unsigned long i = 0; i < fritzBoxes_.size(); ++i) {
        if (i) out << ',';
        const FritzBoxDevice& box = fritzBoxes_[i];
        out << "{\"id\":\"" << jsonEscape(box.id) << "\",\"name\":\"" << jsonEscape(box.name)
            << "\",\"host\":\"" << jsonEscape(box.host) << "\",\"username\":\"" << jsonEscape(box.username)
            << "\",\"clients\":[";
        for (unsigned long j = 0; j < box.clients.size(); ++j) {
            const FritzBoxClient& client = box.clients[j]; if (j) out << ',';
            out << "{\"id\":\"" << jsonEscape(client.id) << "\",\"mac\":\"" << jsonEscape(client.mac)
                << "\",\"name\":\"" << jsonEscape(client.name) << "\"}";
        }
        out << "]}";
    }
    out << ']'; pthread_mutex_unlock(&mutex_); return out.str();
}

namespace {
std::string encodeField(const std::string& value) {
    std::ostringstream out;
    for (unsigned long i = 0; i < value.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') out << c;
        else { static const char hex[] = "0123456789ABCDEF"; out << '%' << hex[c >> 4] << hex[c & 15]; }
    }
    return out.str();
}
std::string decodeField(const std::string& value) {
    std::string out;
    for (unsigned long i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            unsigned int byte = 0; std::istringstream in(value.substr(i + 1, 2)); in >> std::hex >> byte;
            out += static_cast<char>(byte); i += 2;
        } else out += value[i];
    }
    return out;
}
std::string clientDeviceId(const std::string& mac) {
    std::string digits = "fb";
    for (unsigned long i = 0; i < mac.size(); ++i) if (isxdigit(static_cast<unsigned char>(mac[i]))) digits += static_cast<char>(tolower(static_cast<unsigned char>(mac[i])));
    while (digits.size() < 16) digits += '0';
    return digits.substr(0, 16);
}
}

bool ExternalHttpDeviceWorker::saveFritzBox(const std::string& id, const std::string& name, const std::string& host,
                                            const std::string& username, const std::string& password,
                                            const std::vector<FritzBoxClient>& clients, std::string* resultId, std::string* error) {
    if (name.empty() || name.size() > 80 || host.empty() || host.size() > 200 || clients.empty()) { *error = "invalid Fritz!Box settings or no Wi-Fi clients selected"; return false; }
    FritzBoxDevice box; box.name = name; box.host = host; box.username = username; box.password = password;
    box.id = id;
    if (box.id.empty() && !randomToken(&box.id)) { *error = "cannot generate Fritz!Box id"; return false; }
    box.clients = clients;
    for (unsigned long i = 0; i < box.clients.size(); ++i) {
        box.clients[i].id = clientDeviceId(box.clients[i].mac);
        box.clients[i].mac = lowerAscii(box.clients[i].mac);
        if (box.clients[i].name.empty()) box.clients[i].name = box.clients[i].mac;
        if (gateway_ != 0 && !gateway_->registerShelly(box.clients[i].id, box.clients[i].name, box.clients[i].name,
                "fritz", std::vector<int>(), std::vector<int>(), std::vector<int>(), error)) return false;
    }
    pthread_mutex_lock(&mutex_);
    std::vector<FritzBoxDevice> previous = fritzBoxes_;
    std::vector<FritzBoxClient> oldClients;
    unsigned long index = fritzBoxes_.size();
    for (unsigned long i = 0; i < fritzBoxes_.size(); ++i) if (fritzBoxes_[i].id == id && !id.empty()) { index = i; break; }
    if (index < fritzBoxes_.size()) {
        if (box.password.empty()) box.password = fritzBoxes_[index].password;
        if (box.username.empty()) box.username = fritzBoxes_[index].username;
    }
    if (index < fritzBoxes_.size()) oldClients = fritzBoxes_[index].clients;
    if (index == fritzBoxes_.size()) fritzBoxes_.push_back(box); else fritzBoxes_[index] = box;
    bool saved = persistFritzBoxes(error);
    if (!saved) fritzBoxes_ = previous;
    pthread_cond_signal(&ready_); pthread_mutex_unlock(&mutex_);
    if (!saved) return false;
    for (unsigned long i = 0; gateway_ != 0 && i < oldClients.size(); ++i) {
        bool keep = false;
        for (unsigned long j = 0; j < box.clients.size(); ++j) if (oldClients[i].id == box.clients[j].id) keep = true;
        if (!keep) gateway_->removeShelly(oldClients[i].id, error);
    }
    *resultId = box.id; return true;
}

bool ExternalHttpDeviceWorker::removeFritzBox(const std::string& id, std::string* error) {
    pthread_mutex_lock(&mutex_);
    for (unsigned long i = 0; i < fritzBoxes_.size(); ++i) if (fritzBoxes_[i].id == id) {
        FritzBoxDevice removed = fritzBoxes_[i]; fritzBoxes_.erase(fritzBoxes_.begin() + i);
        if (!persistFritzBoxes(error)) { fritzBoxes_.insert(fritzBoxes_.begin() + i, removed); pthread_mutex_unlock(&mutex_); return false; }
        pthread_mutex_unlock(&mutex_);
        for (unsigned long j = 0; gateway_ != 0 && j < removed.clients.size(); ++j) gateway_->removeShelly(removed.clients[j].id, error);
        return true;
    }
    pthread_mutex_unlock(&mutex_); *error = "unknown Fritz!Box"; return false;
}

bool ExternalHttpDeviceWorker::loadFritzBoxes(std::string* error) {
    std::string filename = runtimeDirectory_ + "/fritz-boxes.tsv";
    std::ifstream input(filename.c_str());
    if (!input) return true;
    if (chmod(filename.c_str(), 0600) != 0) { *error = "cannot secure Fritz!Box credentials"; return false; }
    std::map<std::string, FritzBoxDevice> boxes;
    std::string line;
    while (std::getline(input, line)) {
        std::vector<std::string> fields; std::string::size_type start = 0, end;
        while ((end = line.find('\t', start)) != std::string::npos) { fields.push_back(line.substr(start, end - start)); start = end + 1; }
        fields.push_back(line.substr(start));
        if (fields.size() != 7) { *error = "invalid Fritz!Box configuration"; return false; }
        FritzBoxDevice& box = boxes[decodeField(fields[0])]; box.id = decodeField(fields[0]); box.name = decodeField(fields[1]); box.host = decodeField(fields[2]); box.username = decodeField(fields[3]); box.password = decodeField(fields[4]);
        FritzBoxClient client; client.mac = decodeField(fields[5]); client.name = decodeField(fields[6]); client.id = clientDeviceId(client.mac); box.clients.push_back(client);
    }
    for (std::map<std::string, FritzBoxDevice>::iterator it = boxes.begin(); it != boxes.end(); ++it) fritzBoxes_.push_back(it->second);
    return true;
}

bool ExternalHttpDeviceWorker::persistFritzBoxes(std::string* error) const {
    std::string filename = runtimeDirectory_ + "/fritz-boxes.tsv";
    std::string temporary = filename + ".tmp";
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { *error = "cannot write Fritz!Box configuration"; return false; }
    for (unsigned long i = 0; i < fritzBoxes_.size(); ++i) for (unsigned long j = 0; j < fritzBoxes_[i].clients.size(); ++j) {
        const FritzBoxDevice& box = fritzBoxes_[i]; const FritzBoxClient& client = box.clients[j];
        std::string line = encodeField(box.id) + "\t" + encodeField(box.name) + "\t" + encodeField(box.host) + "\t" + encodeField(box.username) + "\t" + encodeField(box.password) + "\t" + encodeField(client.mac) + "\t" + encodeField(client.name) + "\n";
        ssize_t written = write(fd, line.data(), line.size());
        if (written != static_cast<ssize_t>(line.size())) { close(fd); unlink(temporary.c_str()); *error = "cannot save Fritz!Box configuration"; return false; }
    }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(temporary.c_str(), filename.c_str()) != 0) { unlink(temporary.c_str()); *error = "cannot atomically save Fritz!Box configuration"; return false; }
    chmod(filename.c_str(), 0600); return true;
}

std::string ExternalHttpDeviceWorker::json(bool includeCallbackUrls) const {
    std::ostringstream out;
    pthread_mutex_lock(&mutex_);
    out << '[';
    for (unsigned long i = 0; i < devices_.size(); ++i) { if (i) out << ','; out << deviceJson(devices_[i], includeCallbackUrls); }
    out << ']';
    pthread_mutex_unlock(&mutex_);
    return out.str();
}

bool ExternalHttpDeviceWorker::saveDevice(const std::string& id, const std::string& name, const std::string& host, const std::string& callbackHost,
                                   const std::string& model, const std::string& username, const std::string& password,
                                   const std::vector<int>& inputIndices, const std::vector<int>& temperatureIndices,
                                   const std::vector<int>& relayIndices, unsigned int pollSeconds,
                                   bool invertContact, bool callbackMode, bool clearAuth, std::string* resultId, std::string* error) {
    if (name.empty() || name.size() > 80 || !validHost(host) ||
        (callbackMode && callbackHost.empty()) || (!callbackHost.empty() && !validHost(callbackHost)) ||
        (model != "uni" && model != "plug" && model != "plugs") ||
        (model == "uni" && inputIndices.empty() && temperatureIndices.empty() && relayIndices.empty()) ||
        (model == "uni" && (!validIndices(inputIndices, 1) || !validIndices(temperatureIndices, 2) || !validIndices(relayIndices, 1))) ||
        (model != "uni" && (!inputIndices.empty() || !temperatureIndices.empty() || !relayIndices.empty())) || pollSeconds < 5 || pollSeconds > 3600 ||
        username.size() > 128 || password.size() > 128 || username.find(':') != std::string::npos ||
        username.find('\r') != std::string::npos || username.find('\n') != std::string::npos || password.find('\r') != std::string::npos ||
        password.find('\n') != std::string::npos) { *error = "invalid Shelly settings"; return false; }
    if (username.empty() != password.empty()) { *error = "HTTP username and password must both be supplied"; return false; }
    ShellyDevice d;
    d.name = name; d.host = host; d.callbackHost = callbackHost; d.model = model; d.username = username; d.password = password;
    d.inputIndices = inputIndices; d.temperatureIndices = temperatureIndices; d.relayIndices = relayIndices;
    d.pollSeconds = pollSeconds; d.invertContact = invertContact;
    pthread_mutex_lock(&mutex_);
    std::vector<ShellyDevice> previous = devices_;
    std::string previousName = name;
    if (id.empty()) {
        if (!randomToken(&d.id)) { pthread_mutex_unlock(&mutex_); *error = "cannot generate Shelly id"; return false; }
        d.id = d.id.substr(0, 16);
    } else {
        if (!validId(id)) { pthread_mutex_unlock(&mutex_); *error = "invalid Shelly id"; return false; }
        d.id = id;
    }
    bool replaced = false;
    for (unsigned long i = 0; i < devices_.size(); ++i) if (devices_[i].id == d.id) {
        previousName = devices_[i].name;
        if (!clearAuth && d.username.empty() && d.password.empty()) { d.username = devices_[i].username; d.password = devices_[i].password; }
        if (callbackMode) d.callbackToken = devices_[i].callbackToken;
        replaced = true; break;
    }
    if (callbackMode && d.callbackToken.empty() && !randomToken(&d.callbackToken)) {
        pthread_mutex_unlock(&mutex_); *error = "cannot generate callback token"; return false;
    }
    if (replaced) {
        for (unsigned long i = 0; i < devices_.size(); ++i) if (devices_[i].id == d.id) { devices_[i] = d; break; }
    } else devices_.push_back(d);
    *resultId = d.id;
    bool saved = persist(error);
    if (!saved) devices_ = previous;
    pthread_cond_signal(&ready_);
    pthread_mutex_unlock(&mutex_);
    if (saved && gateway_ != 0 && !gateway_->registerShelly(d.id, d.name, previousName, d.model,
            d.inputIndices, d.temperatureIndices, d.relayIndices, error)) {
        pthread_mutex_lock(&mutex_); devices_ = previous; persist(error); pthread_mutex_unlock(&mutex_);
        return false;
    }
    return saved;
}

bool ExternalHttpDeviceWorker::saveDevice(const std::string& id, const std::string& name, const std::string& host, const std::string& callbackHost,
                                   const std::string& model, const std::string& username, const std::string& password,
                                   int inputIndex, int temperatureIndex, int relayIndex, unsigned int pollSeconds,
                                   bool invertContact, bool callbackMode, bool clearAuth, std::string* resultId, std::string* error) {
    std::vector<int> inputs, temperatures, relays;
    if (inputIndex >= 0) inputs.push_back(inputIndex);
    if (temperatureIndex >= 0) temperatures.push_back(temperatureIndex);
    if (model == "uni" && relayIndex >= 0) relays.push_back(relayIndex);
    return saveDevice(id, name, host, callbackHost, model, username, password, inputs, temperatures, relays,
                      pollSeconds, invertContact, callbackMode, clearAuth, resultId, error);
}

bool ExternalHttpDeviceWorker::removeDevice(const std::string& id, std::string* error) {
    pthread_mutex_lock(&mutex_);
    for (unsigned long i = 0; i < devices_.size(); ++i) if (devices_[i].id == id) {
        std::vector<ShellyDevice> previous = devices_;
        devices_.erase(devices_.begin() + i);
        bool saved = persist(error);
        if (!saved) devices_ = previous;
        pthread_mutex_unlock(&mutex_);
        if (saved && gateway_ != 0 && !gateway_->removeShelly(id, error)) {
            pthread_mutex_lock(&mutex_); devices_ = previous; persist(error); pthread_mutex_unlock(&mutex_);
            return false;
        }
        return saved;
    }
    pthread_mutex_unlock(&mutex_); *error = "unknown Shelly device"; return false;
}

bool ExternalHttpDeviceWorker::setActor(const std::string& id, int relayIndex, bool on, std::string* error) {
    pthread_mutex_lock(&mutex_);
    for (unsigned long i = 0; i < devices_.size(); ++i) if (devices_[i].id == id) {
        if (devices_[i].model == "uni" &&
            std::find(devices_[i].relayIndices.begin(), devices_[i].relayIndices.end(), relayIndex) == devices_[i].relayIndices.end()) {
            pthread_mutex_unlock(&mutex_); *error = "Shelly relay is not selected"; return false;
        }
        for (unsigned long j = 0; j < commands_.size(); ++j) if (commands_[j].id == id && commands_[j].relayIndex == relayIndex) {
            commands_[j].on = on;
            pthread_mutex_unlock(&mutex_); return true;
        }
        if (commands_.size() >= 32) {
            pthread_mutex_unlock(&mutex_); *error = "Shelly actor queue is full"; return false;
        }
        ActorCommand command; command.id = id; command.relayIndex = relayIndex; command.on = on; commands_.push_back(command);
        pthread_cond_signal(&ready_); pthread_mutex_unlock(&mutex_); return true;
    }
    pthread_mutex_unlock(&mutex_); *error = "unknown Shelly actor"; return false;
}

bool ExternalHttpDeviceWorker::refresh(const std::string& id, std::string* error) {
    pthread_mutex_lock(&mutex_);
    for (unsigned long i = 0; i < devices_.size(); ++i) if (devices_[i].id == id) {
        devices_[i].nextPoll = 0;
        devices_[i].manualRefresh = true;
        pthread_cond_signal(&ready_);
        pthread_mutex_unlock(&mutex_); return true;
    }
    pthread_mutex_unlock(&mutex_); *error = "unknown Shelly device"; return false;
}

bool ExternalHttpDeviceWorker::refreshFritzBox(const std::string& id, std::string* error) {
    pthread_mutex_lock(&mutex_);
    for (unsigned long i = 0; i < fritzBoxes_.size(); ++i)
        for (unsigned long j = 0; j < fritzBoxes_[i].clients.size(); ++j)
            if (fritzBoxes_[i].clients[j].id == id) {
                fritzBoxes_[i].nextPoll = 0;
                pthread_cond_signal(&ready_);
                pthread_mutex_unlock(&mutex_);
                return true;
            }
    pthread_mutex_unlock(&mutex_);
    *error = "unknown FRITZ!Box";
    return false;
}

bool ExternalHttpDeviceWorker::callback(const std::string& id, const std::string& event, const std::string& token, bool* overloaded) {
    if (overloaded != 0) *overloaded = false;
    pthread_mutex_lock(&mutex_);
    for (unsigned long i = 0; i < devices_.size(); ++i) {
        ShellyDevice& d = devices_[i];
        if (d.id != id || d.callbackToken.empty() || d.callbackToken != token) continue;
        CallbackEvent update; update.id = id; update.channelIndex = 0;
        std::string base = event;
        std::string::size_type suffix = event.rfind('-');
        if (suffix != std::string::npos && suffix + 1 < event.size()) {
            char* end = 0; long oneBased = strtol(event.c_str() + suffix + 1, &end, 10);
            if (end != event.c_str() + suffix + 1 && *end == 0 && oneBased >= 1 && oneBased <= 3) {
                update.channelIndex = static_cast<int>(oneBased - 1); base = event.substr(0, suffix);
            }
        }
        bool inputConfigured = std::find(d.inputIndices.begin(), d.inputIndices.end(), update.channelIndex) != d.inputIndices.end();
        bool relayConfigured = std::find(d.relayIndices.begin(), d.relayIndices.end(), update.channelIndex) != d.relayIndices.end();
        if (base == "input-on" && d.model == "uni" && inputConfigured) { update.metric = 1; update.value = !d.invertContact; }
        else if (base == "input-off" && d.model == "uni" && inputConfigured) { update.metric = 1; update.value = d.invertContact; }
        else if (base == "relay-on" && (d.model != "uni" || relayConfigured)) { update.metric = 3; update.value = 1; }
        else if (base == "relay-off" && (d.model != "uni" || relayConfigured)) { update.metric = 3; update.value = 0; }
        else { pthread_mutex_unlock(&mutex_); return false; }
        if (callbacks_.size() >= 32) { if (overloaded != 0) *overloaded = true; pthread_mutex_unlock(&mutex_); return false; }
        callbacks_.push_back(update);
        pthread_cond_signal(&ready_);
        pthread_mutex_unlock(&mutex_); return true;
    }
    pthread_mutex_unlock(&mutex_); return false;
}

bool ExternalHttpDeviceWorker::load(std::string* error) {
    std::string filename = runtimeDirectory_ + "/shelly-devices.jsonl";
    std::ifstream input(filename.c_str());
    if (input && chmod(filename.c_str(), 0600) != 0) { *error = "cannot secure Shelly credentials"; return false; }
    std::string line;
    std::vector<ShellyDevice> loaded;
    while (std::getline(input, line)) {
        ShellyDevice d;
        long value = 0;
        if (!jsonStringField(line, "id", &d.id) || !jsonStringField(line, "name", &d.name) ||
            !jsonStringField(line, "host", &d.host) || !jsonStringField(line, "model", &d.model) || !validId(d.id) || !validHost(d.host) ||
            (d.model != "uni" && d.model != "plug" && d.model != "plugs")) continue;
        jsonStringField(line, "username", &d.username); jsonStringField(line, "password", &d.password);
        jsonStringField(line, "callback_host", &d.callbackHost);
        if (!d.callbackHost.empty() && !validHost(d.callbackHost)) d.callbackHost.clear();
        jsonStringField(line, "callback_token", &d.callbackToken);
        std::string indexList;
        if (jsonStringField(line, "input_indices", &indexList)) parseIndices(indexList, 1, true, &d.inputIndices);
        else if (jsonLongField(line, "input_index", &value) && value >= 0 && value <= 1) d.inputIndices.push_back(static_cast<int>(value));
        if (jsonStringField(line, "temperature_indices", &indexList)) parseIndices(indexList, 2, true, &d.temperatureIndices);
        else if (jsonLongField(line, "temperature_index", &value) && value >= 0 && value <= 2) d.temperatureIndices.push_back(static_cast<int>(value));
        if (jsonStringField(line, "relay_indices", &indexList)) parseIndices(indexList, 1, true, &d.relayIndices);
        else if (jsonLongField(line, "relay_index", &value) && value >= 0 && value <= 1) d.relayIndices.push_back(static_cast<int>(value));
        if (jsonLongField(line, "poll_seconds", &value) && value >= 5 && value <= 3600) d.pollSeconds = static_cast<unsigned int>(value);
        d.invertContact = line.find("\"invert_contact\":true") != std::string::npos;
        loaded.push_back(d);
    }
    pthread_mutex_lock(&mutex_); devices_ = loaded; pthread_mutex_unlock(&mutex_);
    return true;
}

bool ExternalHttpDeviceWorker::persist(std::string* error) const {
    std::string filename = runtimeDirectory_ + "/shelly-devices.jsonl";
    std::string temporary = filename + ".tmp";
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { *error = "cannot write Shelly configuration"; return false; }
    fchmod(fd, 0600);
    std::ostringstream output;
    for (unsigned long i = 0; i < devices_.size(); ++i) {
        const ShellyDevice& d = devices_[i];
        output << "{\"id\":\"" << jsonEscape(d.id) << "\",\"name\":\"" << jsonEscape(d.name)
               << "\",\"host\":\"" << jsonEscape(d.host) << "\",\"model\":\"" << jsonEscape(d.model)
               << "\",\"username\":\"" << jsonEscape(d.username) << "\",\"password\":\"" << jsonEscape(d.password)
               << "\",\"callback_token\":\"" << jsonEscape(d.callbackToken)
               << "\",\"callback_host\":\"" << jsonEscape(d.callbackHost) << "\",\"input_index\":" << firstIndex(d.inputIndices, -1)
               << ",\"temperature_index\":" << firstIndex(d.temperatureIndices, -1) << ",\"relay_index\":" << firstIndex(d.relayIndices, 0)
               << ",\"input_indices\":\"" << indicesText(d.inputIndices) << "\",\"temperature_indices\":\""
               << indicesText(d.temperatureIndices) << "\",\"relay_indices\":\"" << indicesText(d.relayIndices) << "\""
               << ",\"poll_seconds\":" << d.pollSeconds << ",\"invert_contact\":" << (d.invertContact ? "true" : "false") << "}\n";
    }
    std::string bytes = output.str();
    unsigned long written = 0;
    while (written < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + written, bytes.size() - written);
        if (n <= 0) { close(fd); unlink(temporary.c_str()); *error = "cannot save Shelly configuration"; return false; }
        written += static_cast<unsigned long>(n);
    }
    int syncResult = fsync(fd);
    int closeResult = close(fd);
    if (syncResult != 0 || closeResult != 0 || rename(temporary.c_str(), filename.c_str()) != 0) {
        unlink(temporary.c_str()); *error = "cannot atomically save Shelly configuration"; return false;
    }
    return chmod(filename.c_str(), 0600) == 0;
}
