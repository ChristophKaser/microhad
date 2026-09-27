#include "shelly_integration.h"

#include "json_util.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
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

bool httpGet(const ShellyDevice& device, const std::string& path, std::string* body) {
    std::string host = device.host;
    unsigned short port = 80;
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
    request << "GET " << path << " HTTP/1.1\r\nHost: " << device.host << "\r\nConnection: close\r\n";
    if (!device.username.empty()) request << "Authorization: Basic " << base64(device.username + ":" + device.password) << "\r\n";
    request << "\r\n";
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
    if (lineEnd == std::string::npos || (response.compare(0, 12, "HTTP/1.1 200") != 0 &&
        response.compare(0, 12, "HTTP/1.0 200") != 0)) return false;
    std::string::size_type headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return false;
    *body = response.substr(headerEnd + 4);
    return true;
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

std::string callbackUrl(const ShellyDevice& device, const char* event) {
    return std::string("http://<gateway-ip>/api/shelly/callback/") + device.id + "/" + event + "?token=" + device.callbackToken;
}

std::string deviceJson(const ShellyDevice& d, bool includeCallbackUrls) {
    std::ostringstream out;
    out << "{\"id\":\"" << jsonEscape(d.id) << "\",\"name\":\"" << jsonEscape(d.name)
        << "\",\"host\":\"" << jsonEscape(d.host) << "\",\"model\":\"" << jsonEscape(d.model)
        << "\",\"http_auth\":" << (!d.username.empty() ? "true" : "false")
        << "\",\"input_index\":" << d.inputIndex << ",\"temperature_index\":" << d.temperatureIndex
        << ",\"relay_index\":" << d.relayIndex << ",\"poll_seconds\":" << d.pollSeconds
        << ",\"invert_contact\":" << (d.invertContact ? "true" : "false")
        << ",\"callback_mode\":" << (d.callbackToken.empty() ? "false" : "true")
        << ",\"has_contact\":" << (d.hasContact ? "true" : "false") << ",\"contact_known\":" << (d.contactKnown ? "true" : "false")
        << ",\"contact_open\":" << (d.contactOpen ? "true" : "false")
        << ",\"has_temperature\":" << (d.hasTemperature ? "true" : "false") << ",\"temperature_known\":" << (d.temperatureKnown ? "true" : "false")
        << ",\"temperature_celsius\":" << d.temperatureCelsius
        << ",\"has_actor\":" << (d.hasActor ? "true" : "false") << ",\"actor_known\":" << (d.actorKnown ? "true" : "false")
        << ",\"actor_on\":" << (d.actorOn ? "true" : "false")
        << ",\"last_seen\":" << d.lastSeen;
    if (includeCallbackUrls && !d.callbackToken.empty()) {
        out << ",\"callback_urls\":{\"input_open\":\"" << jsonEscape(callbackUrl(d, "input-on"))
            << "\",\"input_closed\":\"" << jsonEscape(callbackUrl(d, "input-off"))
            << "\",\"relay_on\":\"" << jsonEscape(callbackUrl(d, "relay-on"))
            << "\",\"relay_off\":\"" << jsonEscape(callbackUrl(d, "relay-off")) << "\"}";
    }
    out << "}";
    return out.str();
}

}

ShellyDevice::ShellyDevice() : inputIndex(0), temperatureIndex(0), relayIndex(0), pollSeconds(30), hasContact(false),
    contactKnown(false), contactOpen(false), invertContact(false), hasTemperature(false), temperatureKnown(false),
    temperatureCelsius(0), hasActor(false), actorKnown(false), actorOn(false), lastSeen(0), nextPoll(0) {}

ShellyIntegration::ShellyIntegration(const std::string& runtimeDirectory) : runtimeDirectory_(runtimeDirectory), running_(false), started_(false) {
    pthread_mutex_init(&mutex_, 0);
    pthread_cond_init(&ready_, 0);
}

ShellyIntegration::~ShellyIntegration() {
    stop();
    pthread_cond_destroy(&ready_);
    pthread_mutex_destroy(&mutex_);
}

bool ShellyIntegration::start(std::string* error) {
    if (!load(error)) return false;
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

void ShellyIntegration::stop() {
    pthread_mutex_lock(&mutex_);
    if (!started_) { pthread_mutex_unlock(&mutex_); return; }
    running_ = false;
    pthread_cond_broadcast(&ready_);
    pthread_mutex_unlock(&mutex_);
    pthread_join(thread_, 0);
    pthread_mutex_lock(&mutex_); started_ = false; pthread_mutex_unlock(&mutex_);
}

void* ShellyIntegration::threadMain(void* opaque) { static_cast<ShellyIntegration*>(opaque)->run(); return 0; }

void ShellyIntegration::run() {
    while (true) {
        pthread_mutex_lock(&mutex_);
        if (!running_) { pthread_mutex_unlock(&mutex_); break; }
        pthread_mutex_unlock(&mutex_);
        executeCommands();
        pollDevices();
        pthread_mutex_lock(&mutex_);
        if (running_) {
            struct timespec deadline;
            deadline.tv_sec = time(0) + 1; deadline.tv_nsec = 0;
            pthread_cond_timedwait(&ready_, &mutex_, &deadline);
        }
        pthread_mutex_unlock(&mutex_);
    }
}

void ShellyIntegration::executeCommands() {
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
            std::ostringstream path; path << "/relay/" << device.relayIndex << "?turn=" << (commands[i].on ? "on" : "off");
            std::string response;
            if (httpGet(device, path.str(), &response)) {
                bool state = false;
                if (jsonBoolField(response, "ison", &state)) {
                    pthread_mutex_lock(&mutex_);
                    for (unsigned long j = 0; j < devices_.size(); ++j) if (devices_[j].id == device.id) {
                        devices_[j].actorOn = state; devices_[j].actorKnown = true;
                        devices_[j].lastSeen = static_cast<long>(time(0)); break;
                    }
                    pthread_mutex_unlock(&mutex_);
                }
            }
        }
    }
}

void ShellyIntegration::pollDevices() {
    std::vector<ShellyDevice> snapshot;
    long now = static_cast<long>(time(0));
    pthread_mutex_lock(&mutex_);
    for (unsigned long i = 0; i < devices_.size(); ++i) {
        if (devices_[i].nextPoll <= now) devices_[i].nextPoll = now + static_cast<long>(devices_[i].pollSeconds);
    }
    snapshot = devices_;
    pthread_mutex_unlock(&mutex_);
    for (unsigned long i = 0; i < snapshot.size(); ++i) {
        ShellyDevice& d = snapshot[i];
        if ((!d.callbackToken.empty() && (d.model != "uni" || d.temperatureIndex < 0)) ||
            d.nextPoll != now + static_cast<long>(d.pollSeconds)) continue;
        std::string body;
        if (!httpGet(d, "/status", &body)) continue;
        bool hasContact = false, contactOpen = false, hasActor = false, actorOn = false;
        double temperature = 0;
        if (d.model == "uni") {
            std::string input = objectAt(body, "inputs", d.inputIndex);
            long raw = -1;
            hasContact = !input.empty() && jsonLongField(input, "input", &raw) && (raw == 0 || raw == 1);
            contactOpen = (raw != 0) != d.invertContact;
            std::string relay = objectAt(body, "relays", d.relayIndex);
            hasActor = !relay.empty() && jsonBoolField(relay, "ison", &actorOn);
            double value = 0;
            if (d.temperatureIndex >= 0 && extractIndexedNumber(body, "ext_temperature", d.temperatureIndex, "tC", &value)) temperature = value;
        } else {
            std::string relay = objectAt(body, "relays", 0);
            hasActor = !relay.empty() && jsonBoolField(relay, "ison", &actorOn);
        }
        pthread_mutex_lock(&mutex_);
        for (unsigned long j = 0; j < devices_.size(); ++j) if (devices_[j].id == d.id) {
            ShellyDevice& current = devices_[j];
            bool callbackMode = !current.callbackToken.empty();
            if ((!callbackMode || current.model != "uni") && hasContact) { current.contactKnown = true; current.contactOpen = contactOpen; }
            if (hasActor && !callbackMode) { current.actorKnown = true; current.actorOn = actorOn; }
            if (d.model == "uni" && d.temperatureIndex >= 0 && extractIndexedNumber(body, "ext_temperature", d.temperatureIndex, "tC", &temperature)) {
                current.temperatureKnown = true; current.temperatureCelsius = temperature;
            }
            current.lastSeen = now;
            break;
        }
        pthread_mutex_unlock(&mutex_);
    }
}

std::string ShellyIntegration::json(bool includeCallbackUrls) const {
    std::ostringstream out;
    pthread_mutex_lock(&mutex_);
    out << '[';
    for (unsigned long i = 0; i < devices_.size(); ++i) { if (i) out << ','; out << deviceJson(devices_[i], includeCallbackUrls); }
    out << ']';
    pthread_mutex_unlock(&mutex_);
    return out.str();
}

bool ShellyIntegration::saveDevice(const std::string& id, const std::string& name, const std::string& host,
                                   const std::string& model, const std::string& username, const std::string& password,
                                   int inputIndex, int temperatureIndex, int relayIndex, unsigned int pollSeconds,
                                   bool invertContact, bool callbackMode, bool clearAuth, std::string* resultId, std::string* error) {
    if (name.empty() || name.size() > 80 || !validHost(host) ||
        (model != "uni" && model != "plug" && model != "plugs") ||
        (model == "uni" && (inputIndex < 0 || inputIndex > 1 || temperatureIndex < 0 || temperatureIndex > 2 || relayIndex < 0 || relayIndex > 1)) ||
        (model != "uni" && (inputIndex != -1 || temperatureIndex != -1 || relayIndex != 0)) || pollSeconds < 5 || pollSeconds > 3600 ||
        username.size() > 128 || password.size() > 128 || username.find(':') != std::string::npos ||
        username.find('\r') != std::string::npos || username.find('\n') != std::string::npos || password.find('\r') != std::string::npos ||
        password.find('\n') != std::string::npos) { *error = "invalid Shelly settings"; return false; }
    if (username.empty() != password.empty()) { *error = "HTTP username and password must both be supplied"; return false; }
    ShellyDevice d;
    d.name = name; d.host = host; d.model = model; d.username = username; d.password = password;
    d.inputIndex = inputIndex; d.temperatureIndex = temperatureIndex; d.relayIndex = relayIndex;
    d.pollSeconds = pollSeconds; d.invertContact = invertContact;
    pthread_mutex_lock(&mutex_);
    std::vector<ShellyDevice> previous = devices_;
    if (id.empty()) {
        if (!randomToken(&d.id)) { pthread_mutex_unlock(&mutex_); *error = "cannot generate Shelly id"; return false; }
        d.id = d.id.substr(0, 16);
    } else {
        if (!validId(id)) { pthread_mutex_unlock(&mutex_); *error = "invalid Shelly id"; return false; }
        d.id = id;
    }
    bool replaced = false;
    for (unsigned long i = 0; i < devices_.size(); ++i) if (devices_[i].id == d.id) {
        if (!clearAuth && d.username.empty() && d.password.empty()) { d.username = devices_[i].username; d.password = devices_[i].password; }
        if (callbackMode) d.callbackToken = devices_[i].callbackToken;
        d.contactKnown = devices_[i].contactKnown; d.contactOpen = devices_[i].contactOpen;
        d.temperatureKnown = devices_[i].temperatureKnown; d.temperatureCelsius = devices_[i].temperatureCelsius;
        d.actorKnown = devices_[i].actorKnown; d.actorOn = devices_[i].actorOn; d.lastSeen = devices_[i].lastSeen;
        replaced = true; break;
    }
    if (callbackMode && d.callbackToken.empty() && !randomToken(&d.callbackToken)) {
        pthread_mutex_unlock(&mutex_); *error = "cannot generate callback token"; return false;
    }
    d.hasContact = model == "uni";
    d.hasTemperature = model == "uni" && temperatureIndex >= 0;
    d.hasActor = true;
    if (replaced) {
        for (unsigned long i = 0; i < devices_.size(); ++i) if (devices_[i].id == d.id) { devices_[i] = d; break; }
    } else devices_.push_back(d);
    *resultId = d.id;
    bool saved = persist(error);
    if (!saved) devices_ = previous;
    pthread_cond_signal(&ready_);
    pthread_mutex_unlock(&mutex_);
    return saved;
}

bool ShellyIntegration::removeDevice(const std::string& id, std::string* error) {
    pthread_mutex_lock(&mutex_);
    for (unsigned long i = 0; i < devices_.size(); ++i) if (devices_[i].id == id) {
        std::vector<ShellyDevice> previous = devices_;
        devices_.erase(devices_.begin() + i);
        bool saved = persist(error);
        if (!saved) devices_ = previous;
        pthread_mutex_unlock(&mutex_); return saved;
    }
    pthread_mutex_unlock(&mutex_); *error = "unknown Shelly device"; return false;
}

bool ShellyIntegration::setActor(const std::string& id, bool on, std::string* error) {
    pthread_mutex_lock(&mutex_);
    for (unsigned long i = 0; i < devices_.size(); ++i) if (devices_[i].id == id && devices_[i].hasActor) {
        ActorCommand command; command.id = id; command.on = on; commands_.push_back(command);
        pthread_cond_signal(&ready_); pthread_mutex_unlock(&mutex_); return true;
    }
    pthread_mutex_unlock(&mutex_); *error = "unknown Shelly actor"; return false;
}

bool ShellyIntegration::callback(const std::string& id, const std::string& event, const std::string& token) {
    pthread_mutex_lock(&mutex_);
    for (unsigned long i = 0; i < devices_.size(); ++i) {
        ShellyDevice& d = devices_[i];
        if (d.id != id || d.callbackToken.empty() || d.callbackToken != token) continue;
        if (event == "input-on" && d.model == "uni") { d.contactKnown = true; d.contactOpen = !d.invertContact; }
        else if (event == "input-off" && d.model == "uni") { d.contactKnown = true; d.contactOpen = d.invertContact; }
        else if (event == "relay-on") { d.actorKnown = true; d.actorOn = true; }
        else if (event == "relay-off") { d.actorKnown = true; d.actorOn = false; }
        else { pthread_mutex_unlock(&mutex_); return false; }
        d.lastSeen = static_cast<long>(time(0));
        pthread_mutex_unlock(&mutex_); return true;
    }
    pthread_mutex_unlock(&mutex_); return false;
}

bool ShellyIntegration::load(std::string* error) {
    std::string filename = runtimeDirectory_ + "/shelly-devices.jsonl";
    std::ifstream input(filename.c_str());
    if (input && chmod(filename.c_str(), 0600) != 0) { *error = "cannot secure Shelly credentials"; return false; }
    std::string line;
    std::vector<ShellyDevice> loaded;
    while (std::getline(input, line)) {
        ShellyDevice d;
        long value = 0;
        if (!jsonStringField(line, "id", &d.id) || !jsonStringField(line, "name", &d.name) ||
            !jsonStringField(line, "host", &d.host) || !jsonStringField(line, "model", &d.model) || d.id.empty() || !validHost(d.host)) continue;
        jsonStringField(line, "username", &d.username); jsonStringField(line, "password", &d.password);
        jsonStringField(line, "callback_token", &d.callbackToken);
        if (jsonLongField(line, "input_index", &value)) d.inputIndex = static_cast<int>(value);
        if (jsonLongField(line, "temperature_index", &value)) d.temperatureIndex = static_cast<int>(value);
        if (jsonLongField(line, "relay_index", &value)) d.relayIndex = static_cast<int>(value);
        if (jsonLongField(line, "poll_seconds", &value) && value >= 5 && value <= 3600) d.pollSeconds = static_cast<unsigned int>(value);
        d.invertContact = line.find("\"invert_contact\":true") != std::string::npos;
        d.hasContact = d.model == "uni";
        d.hasTemperature = d.model == "uni" && d.temperatureIndex >= 0;
        d.hasActor = true;
        loaded.push_back(d);
    }
    pthread_mutex_lock(&mutex_); devices_ = loaded; pthread_mutex_unlock(&mutex_);
    return true;
}

bool ShellyIntegration::persist(std::string* error) const {
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
               << "\",\"callback_token\":\"" << jsonEscape(d.callbackToken) << "\",\"input_index\":" << d.inputIndex
               << ",\"temperature_index\":" << d.temperatureIndex << ",\"relay_index\":" << d.relayIndex
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
