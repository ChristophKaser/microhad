#include "gateway_config.h"
#include "http_server.h"
#include "gateway_runner.h"
#include "storage.h"
#include "json_util.h"
#include "json_writer.h"
#include "proc_stats.h"
#include "web_push.h"
#include "sensor_history.h"
#include "external_http_device.h"
#include "notification_worker.h"
#include "matter_client.h"
#include "matter_worker.h"
#include "automation_worker.h"
#include "syslog_log_reader.h"
#include "thread_name.h"

#include <cerrno>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <netinet/in.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <ctime>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <vector>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <syslog.h>
#endif

#ifndef MICROHAD_BUILD_REVISION
#define MICROHAD_BUILD_REVISION "unknown"
#endif

#ifdef MICROHAD_MBEDTLS_ENABLED
extern "C" int microhad_tls_server_create(const char* certificatePath, const char* privateKeyPath, void** server);
extern "C" void microhad_tls_server_destroy(void* server);
extern "C" int microhad_tls_session_open(void* server, int fd, void** session);
extern "C" int microhad_tls_read_request(void* session, char* request, unsigned long capacity);
extern "C" int microhad_tls_read(void* session, char* output, unsigned long capacity);
extern "C" int microhad_tls_write(void* session, const char* response, unsigned long length);
extern "C" void microhad_tls_session_close(void* session);
#endif

namespace {
const unsigned long maximumRequestBytes = 800000;

enum RequestReadResult {
    RequestReadOk,
    RequestReadMalformed,
    RequestReadTooLarge,
    RequestReadFailed
};

std::string* queuedResponse = 0;
#ifdef MICROHAD_MBEDTLS_ENABLED
pthread_key_t tlsSessionKey;
pthread_once_t tlsSessionKeyOnce = PTHREAD_ONCE_INIT;
void createTlsSessionKey() { pthread_key_create(&tlsSessionKey, 0); }
void* activeTlsSession() { pthread_once(&tlsSessionKeyOnce, createTlsSessionKey); return pthread_getspecific(tlsSessionKey); }
void setActiveTlsSession(void* value) { pthread_once(&tlsSessionKeyOnce, createTlsSessionKey); pthread_setspecific(tlsSessionKey, value); }
#endif

bool writeBytes(int fd, const std::string& bytes) {
#ifdef MICROHAD_MBEDTLS_ENABLED
    if (activeTlsSession() != 0) {
        return microhad_tls_write(activeTlsSession(), bytes.data(), bytes.size()) == 0;
    }
#endif
    const char* cursor = bytes.data();
    unsigned long remaining = bytes.size();
    while (remaining != 0) {
        ssize_t written = write(fd, cursor, remaining);
        if (written <= 0) return false;
        cursor += written;
        remaining -= static_cast<unsigned long>(written);
    }
    return true;
}

#if defined(MICROHAD_MBEDTLS_ENABLED) && MICROHAD_MATTER_ENABLED
/* The Matter worker continues to own WebSocket parsing and protocol state. This
 * thread owns only TLS record translation between its accepted socket and a
 * private Unix socket handed to that worker. */
void relayTls(void* session, int clientFd, int bridgeFd) {
    char buffer[2048];
    while (true) {
        fd_set ready; FD_ZERO(&ready); FD_SET(clientFd, &ready); FD_SET(bridgeFd, &ready);
        int maximum = clientFd > bridgeFd ? clientFd : bridgeFd;
        if (select(maximum + 1, &ready, 0, 0, 0) <= 0) break;
        if (FD_ISSET(clientFd, &ready)) {
            int length = microhad_tls_read(session, buffer, sizeof(buffer));
            if (length <= 0 || write(bridgeFd, buffer, length) != length) break;
        }
        if (FD_ISSET(bridgeFd, &ready)) {
            ssize_t length = read(bridgeFd, buffer, sizeof(buffer));
            if (length <= 0 || microhad_tls_write(session, buffer, static_cast<unsigned long>(length)) != 0) break;
        }
    }
    close(bridgeFd);
}
#endif

void writeResponse(int fd, int status, const char* contentType, const std::string& body, const char* contentEncoding = 0) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << (status == 200 ? " OK" : status == 202 ? " Accepted" : status == 500 ? " Internal Server Error" :
                                           status == 503 ? " Service Unavailable" :
                                           status == 413 ? " Payload Too Large" : status == 404 ? " Not Found" : " Bad Request") << "\r\n"
             << "Content-Type: " << contentType << "\r\n";
    if (contentEncoding != 0) response << "Content-Encoding: " << contentEncoding << "\r\n";
    response << "Cache-Control: no-store\r\nConnection: close\r\n"
             << "Content-Length: " << body.size() << "\r\n\r\n";
    std::string header = response.str();
    if (queuedResponse != 0) {
        *queuedResponse = header + body;
        return;
    }
    if (writeBytes(fd, header)) writeBytes(fd, body);
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
    const unsigned long limit = request.find("POST /api/automations/save ")==0 || request.find("POST /api/automations/validate ")==0 ? maximumRequestBytes : 16384;
    std::string::size_type headerEnd = request.find("\r\n\r\n");
    std::string::size_type lineStart;
    bool sawContentLength = false;
    unsigned long contentLength = 0;
    if (headerEnd == std::string::npos) return RequestReadFailed;
    if (headerEnd + 4 > 16384) return RequestReadTooLarge;
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
    if (contentLength > limit - static_cast<unsigned long>(headerEnd + 4)) return RequestReadTooLarge;
    *expected = static_cast<unsigned long>(headerEnd + 4) + contentLength;
    return RequestReadOk;
}

bool parseShellyIndices(const std::string& text, std::vector<int>* indices) {
    indices->clear();
    if (text.empty()) return true;
    std::string::size_type start = 0;
    while (start < text.size()) {
        std::string::size_type end = text.find(',', start);
        std::string part = text.substr(start, end == std::string::npos ? text.size() - start : end - start);
        if (part.size() != 1 || part[0] < '0' || part[0] > '9') return false;
        int index = part[0] - '0';
        if (std::find(indices->begin(), indices->end(), index) != indices->end()) return false;
        indices->push_back(index);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
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

bool parseLogTimestamp(const std::string& text, unsigned long* timestamp) {
    if (text.empty()) return false;
    unsigned long value = 0;
    for (std::string::size_type i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        unsigned int digit = static_cast<unsigned int>(text[i] - '0');
        if (value > (ULONG_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *timestamp = value;
    return true;
}

bool parseMinutes(const std::string& value, unsigned long* result) {
    char* end = 0;
    errno = 0;
    if (value.empty() || value[0] == '-') return false;
    *result = strtoul(value.c_str(), &end, 10);
    return errno == 0 && end != value.c_str() && *end == 0;
}

void jsonWhitespace(const std::string& text, std::string::size_type* at) {
    while (*at < text.size() && (text[*at] == ' ' || text[*at] == '\t' || text[*at] == '\r' || text[*at] == '\n')) ++*at;
}

bool jsonString(const std::string& text, std::string::size_type* at, std::string* result) {
    if (*at >= text.size() || text[(*at)++] != '"') return false;
    result->clear();
    while (*at < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[(*at)++]);
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c != '\\') { result->push_back(static_cast<char>(c)); continue; }
        if (*at >= text.size()) return false;
        c = static_cast<unsigned char>(text[(*at)++]);
        if (c == '"' || c == '\\' || c == '/') result->push_back(static_cast<char>(c));
        else if (c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't') result->push_back(' ');
        else if (c == 'u') {
            for (int i = 0; i < 4; ++i) {
                if (*at >= text.size() || !((text[*at] >= '0' && text[*at] <= '9') ||
                    (text[*at] >= 'a' && text[*at] <= 'f') || (text[*at] >= 'A' && text[*at] <= 'F'))) return false;
                ++*at;
            }
            result->push_back('?');
        } else return false;
    }
    return false;
}

bool jsonValue(const std::string& text, std::string::size_type* at, unsigned depth, bool* widgets) {
    if (depth > 24) return false;
    jsonWhitespace(text, at);
    if (*at >= text.size()) return false;
    if (text[*at] == '"') { std::string ignored; return jsonString(text, at, &ignored); }
    if (text[*at] == '{') {
        ++*at; jsonWhitespace(text, at);
        if (*at < text.size() && text[*at] == '}') { ++*at; return true; }
        while (*at < text.size()) {
            std::string key;
            if (!jsonString(text, at, &key)) return false;
            jsonWhitespace(text, at);
            if (*at >= text.size() || text[(*at)++] != ':') return false;
            if (depth == 0 && key == "widgets") *widgets = true;
            if (!jsonValue(text, at, depth + 1, widgets)) return false;
            jsonWhitespace(text, at);
            if (*at < text.size() && text[(*at)] == '}') { ++*at; return true; }
            if (*at >= text.size() || text[(*at)++] != ',') return false;
            jsonWhitespace(text, at);
        }
        return false;
    }
    if (text[*at] == '[') {
        ++*at; jsonWhitespace(text, at);
        if (*at < text.size() && text[*at] == ']') { ++*at; return true; }
        while (*at < text.size()) {
            if (!jsonValue(text, at, depth + 1, widgets)) return false;
            jsonWhitespace(text, at);
            if (*at < text.size() && text[*at] == ']') { ++*at; return true; }
            if (*at >= text.size() || text[(*at)++] != ',') return false;
            jsonWhitespace(text, at);
        }
        return false;
    }
    const char* literals[] = { "true", "false", "null" };
    for (unsigned i = 0; i < 3; ++i) {
        std::string literal(literals[i]);
        if (text.compare(*at, literal.size(), literal) == 0) { *at += literal.size(); return true; }
    }
    std::string::size_type start = *at;
    if (text[*at] == '-') ++*at;
    if (*at >= text.size() || text[*at] < '0' || text[*at] > '9') return false;
    if (text[*at] == '0') ++*at;
    else while (*at < text.size() && text[*at] >= '0' && text[*at] <= '9') ++*at;
    if (*at < text.size() && text[*at] == '.') { ++*at; if (*at >= text.size() || text[*at] < '0' || text[*at] > '9') return false; while (*at < text.size() && text[*at] >= '0' && text[*at] <= '9') ++*at; }
    if (*at < text.size() && (text[*at] == 'e' || text[*at] == 'E')) { ++*at; if (*at < text.size() && (text[*at] == '+' || text[*at] == '-')) ++*at; if (*at >= text.size() || text[*at] < '0' || text[*at] > '9') return false; while (*at < text.size() && text[*at] >= '0' && text[*at] <= '9') ++*at; }
    return *at > start;
}

bool dashboardConfigJson(const std::string& value) {
    if (value.size() > 12000) return false;
    std::string::size_type at = 0;
    bool widgets = false;
    jsonWhitespace(value, &at);
    if (at >= value.size() || value[at] != '{' || !jsonValue(value, &at, 0, &widgets)) return false;
    jsonWhitespace(value, &at);
    return widgets && at == value.size();
}

bool saveDashboardConfig(const std::string& filename, const std::string& value) {
    return saveDashboardConfiguration(filename, value);
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

bool readProcLine(const char* path, char* line, unsigned long size) {
    FILE* input = fopen(path, "r");
    if (input == 0) return false;
    bool ok = fgets(line, size, input) != 0;
    fclose(input);
    return ok;
}

bool readCpuCounters(uint64_t* total, uint64_t* idle) {
    char line[1024];
    if (!readProcLine("/proc/stat", line, sizeof(line)) || strncmp(line, "cpu ", 4) != 0) return false;
    const char* cursor = line + 4;
    *total = 0; *idle = 0;
    unsigned int count = 0;
    while (true) {
        while (*cursor == ' ') ++cursor;
        if (*cursor == '\n' || *cursor == 0) break;
        char* end = 0; errno = 0;
        uint64_t value = strtoull(cursor, &end, 10);
        if (errno || end == cursor || (*end && *end != ' ' && *end != '\n')) return false;
        if (count == 3 || count == 4) *idle += value;
        *total += value; ++count; cursor = end;
    }
    return count >= 4;
}

struct MemoryStats {
    uint64_t total, free, reclaimable, cached, slab;
    bool hasTotal, hasFree, hasReclaimable, hasCached, hasSlab;
    MemoryStats() : total(0), free(0), reclaimable(0), cached(0), slab(0),
        hasTotal(false), hasFree(false), hasReclaimable(false), hasCached(false), hasSlab(false) {}
};

MemoryStats readMemory() {
    MemoryStats stats;
    FILE* input = fopen("/proc/meminfo", "r");
    if (input == 0) return stats;
    char line[256];
    while (fgets(line, sizeof(line), input) != 0) {
        char* colon = strchr(line, ':');
        if (colon == 0) continue;
        *colon = 0;
        char* end = 0; errno = 0;
        uint64_t value = strtoull(colon + 1, &end, 10);
        if (errno || end == colon + 1 || strncmp(end, " kB", 3) != 0) continue;
        if (strcmp(line, "MemTotal") == 0) { stats.total = value; stats.hasTotal = true; }
        else if (strcmp(line, "MemFree") == 0) { stats.free = value; stats.hasFree = true; }
        else if (strcmp(line, "MemReclaimable") == 0) { stats.reclaimable = value; stats.hasReclaimable = true; }
        else if (strcmp(line, "Cached") == 0) { stats.cached = value; stats.hasCached = true; }
        else if (strcmp(line, "SReclaimable") == 0) { stats.slab = value; stats.hasSlab = true; }
    }
    fclose(input);
    return stats;
}

bool readProcessStat(const char* path, ProcessStat* stats) {
    char line[4096];
    return readProcLine(path, line, sizeof(line)) && parseProcessStat(line, stats);
}

bool readUptime(double* uptime) {
    char line[128]; double idle;
    return readProcLine("/proc/uptime", line, sizeof(line)) && sscanf(line, "%lf %lf", uptime, &idle) == 2;
}

uint64_t fileBytes(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && info.st_size >= 0 ? static_cast<uint64_t>(info.st_size) : 0;
}

struct ThreadCpuReading {
    uint64_t tid;
    std::string process;
    uint64_t startTicks;
    uint64_t cpuTicks;
    double usage;
    bool sampled;
    std::string name;
};

bool numericName(const char* name, uint64_t* value) {
    char* end = 0;
    if (!name || !*name) return false;
    errno = 0;
    unsigned long parsed = strtoul(name, &end, 10);
    if (errno || end == name || *end) return false;
    *value = parsed;
    return true;
}

void collectProcessThreads(uint64_t pid, const char* process, const ProcessStat& leader,
                           std::map<uint64_t, ThreadCpuReading>* readings) {
    ThreadCpuReading reading;
    reading.tid = pid; reading.process = process; reading.startTicks = leader.startTicks;
    reading.cpuTicks = leader.cpuTicks; reading.name = leader.name; reading.usage = 0; reading.sampled = false;
    (*readings)[pid] = reading;
    char root[96]; snprintf(root, sizeof(root), "/proc/%lu/task", static_cast<unsigned long>(pid));
    DIR* tasks = opendir(root);
    if (tasks == 0) return;
    struct dirent* entry;
    while ((entry = readdir(tasks)) != 0) {
        uint64_t tid;
        if (!numericName(entry->d_name, &tid) || tid == pid || readings->find(tid) != readings->end()) continue;
        char path[160]; snprintf(path, sizeof(path), "%s/%lu/stat", root, static_cast<unsigned long>(tid));
        ProcessStat task;
        if (!readProcessStat(path, &task)) continue;
        reading.tid = tid; reading.startTicks = task.startTicks; reading.cpuTicks = task.cpuTicks; reading.name = task.name;
        (*readings)[tid] = reading;
    }
    closedir(tasks);
}

void collectProcesses(unsigned long processId, const ProcessStat& self, long pageSize,
                      std::map<uint64_t, ThreadCpuReading>* readings,
                      uint64_t* zigguratMemory, bool* hasZigguratMemory,
                      uint64_t* matterMemory, bool* hasMatterMemory) {
    char ownPath[96], ownExecutable[512];
    snprintf(ownPath, sizeof(ownPath), "/proc/%lu/exe", processId);
    ssize_t ownLength = readlink(ownPath, ownExecutable, sizeof(ownExecutable) - 1);
    if (ownLength >= 0) ownExecutable[ownLength] = 0;
    // A single /proc scan handles both LinuxThreads PIDs and NPTL task directories.
    // Each service's RSS is a maximum, since LinuxThreads shares its address space.
    DIR* directory = opendir("/proc");
    if (directory == 0) return;
    struct dirent* entry;
    while ((entry = readdir(directory)) != 0) {
        uint64_t pid;
        if (!numericName(entry->d_name, &pid)) continue;
        char path[96], executable[512];
        snprintf(path, sizeof(path), "/proc/%lu/exe", static_cast<unsigned long>(pid));
        ssize_t length = readlink(path, executable, sizeof(executable) - 1);
        if (length < 0) continue;
        executable[length] = 0;
        const char* base = strrchr(executable, '/'); base = base == 0 ? executable : base + 1;
        const char* process = 0;
        if (ownLength >= 0 && strcmp(executable, ownExecutable) == 0) process = "microhad";
        else if (strcmp(base, "ziggurat") == 0) process = "ziggurat";
        else if (strcmp(base, "mthread-matter") == 0) process = "mthread-matter";
        if (process == 0) continue;
        snprintf(path, sizeof(path), "/proc/%lu/stat", static_cast<unsigned long>(pid));
        ProcessStat stats;
        if (!readProcessStat(path, &stats)) continue;
        if (strcmp(process, "microhad") == 0 && stats.group != self.group) continue;
        if (pageSize > 0 && strcmp(process, "microhad") != 0) {
            uint64_t rss = stats.rssPages * static_cast<uint64_t>(pageSize);
            if (strcmp(process, "ziggurat") == 0) { *hasZigguratMemory = true; *zigguratMemory = std::max(*zigguratMemory, rss); }
            else { *hasMatterMemory = true; *matterMemory = std::max(*matterMemory, rss); }
        }
        collectProcessThreads(pid, process, stats, readings);
    }
    closedir(directory);
}

bool threadUsageOrder(const ThreadCpuReading& left, const ThreadCpuReading& right) {
    if (left.process != right.process) return left.process < right.process;
    if (left.sampled != right.sampled) return left.sampled;
    if (left.sampled && left.usage != right.usage) return left.usage > right.usage;
    if (left.name != right.name) return left.name < right.name;
    return left.tid < right.tid;
}

}

HttpServer::HttpServer(int port, GatewayRunner* gateway, const std::string& webuiDirectory,
                       const std::string& certificatePath, const std::string& privateKeyPath,
                       ExternalHttpDeviceWorker* shelly, NotificationWorker* notifications, MatterWorker* matterWorker, const std::string& companionConfig, AutomationWorker* automations) :
    companionConfig_(companionConfig.empty() ? GatewayConfig().companionJson() : companionConfig), port_(port), listenFd_(-1), gateway_(gateway), processId_(static_cast<unsigned long>(getpid())),
    threadNamePrefix_(certificatePath.empty() ? "http" : "https"), shelly_(shelly), notifications_(notifications),
    matter_(new MatterClient("/tmp/microhad-matter.sock")), matterWorker_(matterWorker), automations_(automations), webPush_(new WebPushStore(gateway->runtimeDirectory())), webuiDirectory_(webuiDirectory),
    certificatePath_(certificatePath), privateKeyPath_(privateKeyPath), tlsServer_(0), running_(false),
    previousCpuTotal_(0), previousCpuIdle_(0), hasPreviousCpu_(false), previousThreadSampleUptime_(0) {
    pthread_mutex_init(&queueMutex_, 0);
    pthread_cond_init(&queueReady_, 0);
    pthread_mutex_init(&statsMutex_, 0);
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
    pthread_mutex_destroy(&statsMutex_);
#ifdef MICROHAD_MBEDTLS_ENABLED
    if (tlsServer_ != 0) microhad_tls_server_destroy(tlsServer_);
#endif
    delete webPush_;
    delete matter_;
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

void* HttpServer::acceptThreadMain(void* opaque) {
    HttpServer* server = static_cast<HttpServer*>(opaque);
    std::string name = server->threadNamePrefix_ + "-accept";
    setCurrentThreadName(name.c_str());
    server->acceptLoop();
    return 0;
}
void* HttpServer::workerThreadMain(void* opaque) {
    HttpServer* server = static_cast<HttpServer*>(opaque);
    std::string name = server->threadNamePrefix_ + "-worker";
    setCurrentThreadName(name.c_str());
#if defined(__linux__)
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10) != 0)
        syslog(LOG_WARNING, "Cannot lower %s priority: errno=%d", name.c_str(), errno);
#endif
    server->workerLoop();
    return 0;
}

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
        unsigned long expected=0;
        if(expectedRequestLength(request,&expected)==RequestReadTooLarge){writeResponse(clientFd,413,"application/json","{\"error\":\"request is too large\"}");setActiveTlsSession(0);microhad_tls_session_close(session);return;}
    } else readResult = readPlainRequest(clientFd, &request);
#else
    readResult = readPlainRequest(clientFd, &request);
#endif
    if (
#ifdef MICROHAD_MBEDTLS_ENABLED
        session != 0 ||
#endif
        readResult == RequestReadOk) {
        if (request.find("GET /api/matter/relay ") == 0) {
#if MICROHAD_MATTER_ENABLED
            std::string upgrade, error;
            if (!MatterWebSocket::handshake(request, "/api/matter/relay", &upgrade))
                writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"malformed\"}");
            else if (!matterWorker_)
                writeResponse(clientFd, 503, "application/json", "{\"ok\":false,\"error\":\"not_ready\"}");
#ifdef MICROHAD_MBEDTLS_ENABLED
            else if (session != 0) {
                int bridge[2] = {-1, -1};
                if (socketpair(AF_UNIX, SOCK_STREAM, 0, bridge) != 0 ||
                    !matterWorker_->adoptRelay(bridge[1], upgrade, &error)) {
                    if (bridge[0] >= 0) close(bridge[0]);
                    if (bridge[1] >= 0) close(bridge[1]);
                    writeResponse(clientFd, 503, "application/json", "{\"ok\":false,\"error\":\"" +
                                  (error.empty() ? "not_ready" : error) + "\"}");
                } else {
                    close(bridge[1]); relayTls(session, clientFd, bridge[0]);
                }
            }
#endif
            else if (!matterWorker_->adoptRelay(clientFd, upgrade, &error))
                writeResponse(clientFd, 503, "application/json", "{\"ok\":false,\"error\":\"" +
                              (error.empty() ? "not_ready" : error) + "\"}");
#else
            writeResponse(clientFd, 404, "application/json", "{\"ok\":false,\"error\":\"unsupported\"}");
#endif
        } else handleRequest(clientFd, request);
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
    if (request.find("GET /api/matter/status ") == 0 || request.find("GET /api/thread/status ") == 0) {
#if MICROHAD_MATTER_ENABLED
        std::string status, error;
        bool thread = request.find("GET /api/thread/status ") == 0;
        if (matterWorker_ ? matterWorker_->status(thread, &status, &error) : matter_->status(thread, &status, &error))
            writeResponse(clientFd, 200, "application/json", status);
        else writeResponse(clientFd, 503, "application/json", "{\"available\":false,\"error\":\"" + jsonEscape(error) + "\"}");
#else
        writeResponse(clientFd, 200, "application/json", "{\"enabled\":false,\"available\":false}");
#endif
    } else if (request.find("POST /api/matter/commissioning/start ") == 0 ||
               request.find("POST /api/matter/commissioning/status ") == 0 ||
               request.find("POST /api/matter/commissioning/cancel ") == 0 ||
               request.find("POST /api/matter/devices/read ") == 0 ||
               request.find("POST /api/matter/devices/command ") == 0 ||
               request.find("POST /api/matter/devices/bind ") == 0 ||
               request.find("POST /api/matter/devices/subscriptions ") == 0 ||
               request.find("POST /api/thread/create ") == 0 || request.find("POST /api/thread/import ") == 0 ||
               request.find("POST /api/thread/retry ") == 0) {
#if MICROHAD_MATTER_ENABLED
        const bool threadMutation = request.find("POST /api/thread/create ") == 0 ||
            request.find("POST /api/thread/import ") == 0 || request.find("POST /api/thread/retry ") == 0;
        const bool deviceRead = request.find("POST /api/matter/devices/read ") == 0;
        const bool deviceCommand = request.find("POST /api/matter/devices/command ") == 0;
        const bool deviceSubscriptions = request.find("POST /api/matter/devices/subscriptions ") == 0;
        const bool deviceBind = request.find("POST /api/matter/devices/bind ") == 0;
        const unsigned char opcode = deviceRead || deviceBind || deviceSubscriptions ? 10 : deviceCommand ? 11 : threadMutation ? 9 : request.find("POST /api/matter/commissioning/start ") == 0 ? 3 :
            request.find("POST /api/matter/commissioning/status ") == 0 ? 4 : 5;
        const std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string body = bodyAt == std::string::npos ? "" : request.substr(bodyAt + 4);
        uint32_t id = 0; std::string error;
        bool valid = true;
        if (threadMutation) {
            std::string prepared;
            const std::string action = request.find("POST /api/thread/import ") == 0 ? "import" :
                request.find("POST /api/thread/retry ") == 0 ? "retry" : "create";
            valid = MatterChannel::threadMutation(action, body, &prepared);
            body.swap(prepared);
        } else if (deviceRead || deviceCommand || deviceBind || deviceSubscriptions) {
            std::string prepared;
            valid = deviceSubscriptions ? MatterChannel::deviceSubscriptionsRequest(body, &prepared) :
                deviceRead ? MatterChannel::deviceReadRequest(body, &prepared) : deviceCommand ?
                MatterChannel::deviceCommandRequest(body, &prepared) : MatterChannel::deviceBindingRequest(body, &prepared);
            body.swap(prepared);
        } else if (opcode == 3) {
            GatewayRunner::ViewLease* view = gateway_->acquireView();
            std::string prepared;
            valid = view && MatterChannel::commissioningStartRequest(
                body, view->matterCountryCode(), view->matterRegulatoryLocation(), &prepared);
            delete view;
            if (valid) body.swap(prepared);
        }
        if (!valid || !MatterChannel::validRequest(opcode, body)) error = "malformed";
        else if (!matterWorker_) error = "not_ready";
        else matterWorker_->submit(opcode, body, &id, &error);
        if (id != 0) {
            std::ostringstream out;
            out << "{\"schema\":1,\"ok\":true,\"job\":" << id << ",\"state\":\"queued\"}";
            writeResponse(clientFd, 202, "application/json", out.str());
        } else writeResponse(clientFd, error == "malformed" ? 400 : 503, "application/json",
                            "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
#else
        writeResponse(clientFd, 404, "application/json", "{\"ok\":false,\"error\":\"unsupported\"}");
#endif
    } else if (request.find("GET /api/matter/jobs/") == 0) {
#if MICROHAD_MATTER_ENABLED
        const unsigned long start = std::string("GET /api/matter/jobs/").size();
        const std::string::size_type end = request.find(' ', start);
        const std::string idText = end == std::string::npos ? "" : request.substr(start, end - start);
        bool valid = !idText.empty() && idText.size() <= 10 && idText[0] != '0';
        uint64_t value = 0;
        for (unsigned long i = 0; valid && i < idText.size(); ++i) {
            valid = idText[i] >= '0' && idText[i] <= '9';
            if (valid) value = value * 10 + idText[i] - '0';
        }
        valid = valid && value <= 0xffffffffU;
        std::string json;
        if (!valid) writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"malformed\"}");
        else if (!matterWorker_) writeResponse(clientFd, 503, "application/json", "{\"ok\":false,\"error\":\"not_ready\"}");
        else if (matterWorker_->job(static_cast<uint32_t>(value), &json)) writeResponse(clientFd, 200, "application/json", json);
        else writeResponse(clientFd, 404, "application/json", "{\"ok\":false,\"error\":\"not_found\"}");
#else
        writeResponse(clientFd, 404, "application/json", "{\"ok\":false,\"error\":\"unsupported\"}");
#endif
    } else if (request.find("GET /api/controller-stats ") == 0) {
        writeResponse(clientFd, 200, "application/json", controllerStatsJson());
    } else if (shelly_ != 0 && request.find("GET /api/external/fritz/config ") == 0) {
        writeResponse(clientFd, 200, "application/json", shelly_->fritzJson());
    } else if (shelly_ != 0 && request.find("POST /api/external/fritz/discover ") == 0) {
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string body = bodyAt == std::string::npos ? "" : request.substr(bodyAt + 4);
        std::string host, username, password, error;
        std::vector<FritzBoxClient> clients;
        bool valid = formValue(body, "host", &host);
        formValue(body, "username", &username); formValue(body, "password", &password);
        if (!valid || !shelly_->discoverFritzClients(host, username, password, &clients, &error))
            writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(valid ? error : "Fritz!Box host is required") + "\"}");
        else {
            std::ostringstream out; out << "{\"ok\":true,\"clients\":[";
            for (unsigned long i = 0; i < clients.size(); ++i) { if (i) out << ','; out << "{\"mac\":\"" << jsonEscape(clients[i].mac) << "\",\"name\":\"" << jsonEscape(clients[i].name) << "\",\"connected\":" << (clients[i].connected ? "true" : "false") << ",\"has_signal\":" << (clients[i].hasSignal ? "true" : "false") << ",\"signal_percent\":" << clients[i].signalPercent << "}"; }
            out << "]}"; writeResponse(clientFd, 200, "application/json", out.str());
        }
    } else if (shelly_ != 0 && request.find("POST /api/external/fritz/config ") == 0) {
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string body = bodyAt == std::string::npos ? "" : request.substr(bodyAt + 4);
        std::string id, name, host, username, password, selected, value, error, savedId;
        bool valid = formValue(body, "id", &id) && formValue(body, "name", &name) && formValue(body, "host", &host) && formValue(body, "selected_macs", &selected);
        formValue(body, "username", &username); formValue(body, "password", &password);
        std::vector<FritzBoxClient> discovered, chosen;
        if (valid) valid = shelly_->discoverFritzClients(host, username, password, &discovered, &error);
        std::string::size_type start = 0;
        while (valid && start < selected.size()) {
            std::string::size_type end = selected.find(',', start);
            std::string mac = selected.substr(start, end == std::string::npos ? selected.size() - start : end - start);
            bool found = false;
            for (unsigned long i = 0; i < discovered.size(); ++i) if (discovered[i].mac == mac) { chosen.push_back(discovered[i]); found = true; break; }
            if (!found) { valid = false; error = "selected Fritz!Box client was not found"; }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (!valid || !shelly_->saveFritzBox(id, name, host, username, password, chosen, &savedId, &error))
            writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(valid ? error : (error.empty() ? "invalid Fritz!Box settings" : error)) + "\"}");
        else writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"id\":\"" + jsonEscape(savedId) + "\"}");
    } else if (shelly_ != 0 && request.find("DELETE /api/external/fritz/") == 0) {
        const std::string prefix = "DELETE /api/external/fritz/";
        std::string::size_type end = request.find(' ', prefix.size());
        std::string id = end == std::string::npos ? "" : decodeComponent(request.substr(prefix.size(), end - prefix.size()));
        std::string error;
        if (shelly_->removeFritzBox(id, &error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
    } else if (shelly_ != 0 && request.find("GET /api/shelly/callback/") == 0) {
        const std::string prefix = "GET /api/shelly/callback/";
        std::string::size_type targetEnd = request.find(' ', prefix.size());
        std::string target = targetEnd == std::string::npos ? "" : request.substr(prefix.size(), targetEnd - prefix.size());
        std::string::size_type slash = target.find('/');
        std::string::size_type query = target.find('?');
        std::string id = slash == std::string::npos ? "" : target.substr(0, slash);
        std::string event = slash == std::string::npos ? "" : target.substr(slash + 1, (query == std::string::npos ? target.size() : query) - slash - 1);
        std::string token;
        bool overloaded = false;
        bool valid = query != std::string::npos && formValue(target.substr(query + 1), "token", &token) &&
                     shelly_->callback(id, event, token, &overloaded);
        if (valid) writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else if (overloaded) writeResponse(clientFd, 503, "application/json", "{\"ok\":false,\"error\":\"Shelly callback queue is full\"}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid callback\"}");
    } else if (shelly_ != 0 && request.find("GET /api/shelly/config ") == 0) {
        writeResponse(clientFd, 200, "application/json", shelly_->json(true));
    } else if (shelly_ != 0 && request.find("POST /api/shelly/config ") == 0) {
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string body = bodyAt == std::string::npos ? "" : request.substr(bodyAt + 4);
        std::string id, name, host, callbackHost, model, username, password, value, error;
        long input = -1, temperature = -1, relay = 0, seconds = 30;
        std::vector<int> inputs, temperatures, relays;
        bool invert = false, callbackMode = false, clearAuth = false;
        bool valid = formValue(body, "id", &id) && formValue(body, "name", &name) && formValue(body, "host", &host) &&
                     formValue(body, "model", &model);
        if (valid && formValue(body, "input_indices", &value)) valid = parseShellyIndices(value, &inputs);
        else if (valid && formValue(body, "input_index", &value)) { input = strtol(value.c_str(), 0, 10); if (input >= 0) inputs.push_back(static_cast<int>(input)); }
        if (valid && formValue(body, "temperature_indices", &value)) valid = parseShellyIndices(value, &temperatures);
        else if (valid && formValue(body, "temperature_index", &value)) { temperature = strtol(value.c_str(), 0, 10); if (temperature >= 0) temperatures.push_back(static_cast<int>(temperature)); }
        if (valid && formValue(body, "relay_indices", &value)) valid = parseShellyIndices(value, &relays);
        else if (valid && formValue(body, "relay_index", &value)) { relay = strtol(value.c_str(), 0, 10); if (relay >= 0) relays.push_back(static_cast<int>(relay)); }
        valid = valid && formValue(body, "poll_seconds", &value);
        if (valid) seconds = strtol(value.c_str(), 0, 10);
        formValue(body, "username", &username); formValue(body, "password", &password);
        formValue(body, "callback_host", &callbackHost);
        invert = formValue(body, "invert_contact", &value) && value == "true";
        callbackMode = formValue(body, "callback_mode", &value) && value == "true";
        clearAuth = formValue(body, "clear_auth", &value) && value == "true";
        std::string savedId;
        if (!valid || seconds < 0 || !shelly_->saveDevice(id, name, host, callbackHost, model, username, password,
                inputs, temperatures, relays,
                static_cast<unsigned int>(seconds), invert, callbackMode, clearAuth, &savedId, &error)) {
            writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(valid ? error : "invalid Shelly settings") + "\"}");
        } else writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"id\":\"" + jsonEscape(savedId) + "\"}");
    } else if (shelly_ != 0 && request.find("DELETE /api/shelly/") == 0) {
        const std::string prefix = "DELETE /api/shelly/";
        std::string::size_type end = request.find(' ', prefix.size());
        std::string id = end == std::string::npos ? "" : decodeComponent(request.substr(prefix.size(), end - prefix.size()));
        std::string error;
        if (shelly_->removeDevice(id, &error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
    } else if (shelly_ != 0 && request.find("POST /api/shelly/") == 0 && request.find("/actor ") != std::string::npos) {
        const std::string prefix = "POST /api/shelly/";
        std::string::size_type suffix = request.find("/actor ", prefix.size());
        std::string id = suffix == std::string::npos ? "" : decodeComponent(request.substr(prefix.size(), suffix - prefix.size()));
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string state, error;
        bool valid = bodyAt != std::string::npos && formValue(request.substr(bodyAt + 4), "state", &state) && (state == "on" || state == "off");
        if (valid && shelly_->setActor(id, 0, state == "on", &error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(valid ? error : "invalid actor command") + "\"}");
    } else if ((request.find("POST /api/devices/") == 0 || request.find("GET /api/devices/") == 0) && request.find("/delete ") != std::string::npos) {
        const bool getting = request.find("GET ") == 0;
        const std::string prefix = getting ? "GET /api/devices/" : "POST /api/devices/";
        const std::string::size_type end = request.find("/delete ", prefix.size());
        const std::string address = decodeComponent(request.substr(prefix.size(), end - prefix.size()));
        std::string error, json;
        if (getting) {
            if (gateway_->deviceDeletionStatus(address, &json, &error)) writeResponse(clientFd, 200, "application/json", json);
            else writeResponse(clientFd, 404, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
        } else {
            const std::string::size_type bodyAt = request.find("\r\n\r\n");
            std::string mode;
            const bool valid = bodyAt != std::string::npos && formValue(request.substr(bodyAt + 4), "mode", &mode) && (mode == "local" || mode == "unpair");
            if (!valid) writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"Invalid deletion mode\"}");
            else if (address.substr(0, 7) == "matter:") {
#if MICROHAD_MATTER_ENABLED
                std::string node; uint32_t job = 0;
                if (gateway_->matterDeletionTarget(address, &node, &error) && matterWorker_ && matterWorker_->submit(10,
                    "{\"schema\":1,\"action\":\"delete\",\"node\":\"" + node + "\",\"local\":" + (mode == "local" ? "true" : "false") + "}", &job, &error)) {
                    std::ostringstream out; out << "{\"ok\":true,\"state\":\"pending\",\"job\":" << job << '}';
                    writeResponse(clientFd, 200, "application/json", out.str());
                } else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error.empty() ? "Matter service unavailable" : error) + "\"}");
#else
                writeResponse(clientFd, 404, "application/json", "{\"ok\":false,\"error\":\"Matter is disabled\"}");
#endif
            } else if (gateway_->deleteDevice(address, mode == "local", &error)) {
                writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"state\":\"pending\"}");
            } else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
        }
    } else if (request.find("GET /api/dashboard/config ") == 0) {
        std::string content;
        if (!readTextFile(gateway_->runtimeDirectory() + "/dashboard.json", &content)) content = "{\"widgets\":[]}";
        if (!dashboardConfigJson(content)) content = "{\"widgets\":[]}";
        writeResponse(clientFd, 200, "application/json", content);
    } else if (request.find("POST /api/dashboard/config ") == 0) {
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string body = bodyAt == std::string::npos ? "" : request.substr(bodyAt + 4);
        if (!dashboardConfigJson(body))
            writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid dashboard configuration\"}");
        else if (!saveDashboardConfig(gateway_->runtimeDirectory() + "/dashboard.json", body))
            writeResponse(clientFd, 500, "application/json", "{\"ok\":false,\"error\":\"cannot save dashboard configuration\"}");
        else writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
    } else if (request.find("GET /api/settings ") == 0) {
        GatewayRunner::ViewLease* view=gateway_->acquireView();
        if (!view) writeResponse(clientFd,503,"application/json","{\"ok\":false,\"error\":\"gateway unavailable\"}");
        else { std::string body=view->settingsJson(); delete view; writeResponse(clientFd,200,"application/json",body); }
    } else if (request.find("POST /api/settings ") == 0) {
        std::string::size_type bodyAt=request.find("\r\n\r\n");
        std::string value,error,country,location;
        const std::string form=bodyAt==std::string::npos ? "" : request.substr(bodyAt+4);
        const bool hasQuorum=formValue(form,"broadcast_passive_ack_quorum",&value);
        const bool hasCountry=formValue(form,"matter_country_code",&country);
        const bool hasLocation=formValue(form,"matter_regulatory_location",&location);
        bool valid=bodyAt!=std::string::npos && (hasQuorum || (hasCountry && hasLocation));
        if (hasCountry != hasLocation) valid=false;
        unsigned int number=0;
        for (unsigned long i=0; valid && hasQuorum && i<value.size(); ++i) {
            if (value[i]<'0' || value[i]>'9') valid=false;
            else number=number*10+static_cast<unsigned int>(value[i]-'0');
        }
        if (valid && hasQuorum && (value.empty() || value.size()>3 || number<1 || number>255)) valid=false;
        if (valid && hasCountry && (country.size()!=2 || country[0]<'A' || country[0]>'Z' ||
            country[1]<'A' || country[1]>'Z' || (location!="indoor" && location!="outdoor"))) valid=false;
        if (!valid)
            writeResponse(clientFd,400,"application/json","{\"ok\":false,\"error\":\"invalid gateway settings\"}");
        else if (hasCountry && !gateway_->setMatterCommissioningRegion(country,location,&error))
            writeResponse(clientFd,400,"application/json","{\"ok\":false,\"error\":\""+jsonEscape(error)+"\"}");
        else if (hasQuorum && !gateway_->setBroadcastQuorum(number,&error))
            writeResponse(clientFd,400,"application/json","{\"ok\":false,\"error\":\""+jsonEscape(error)+"\"}");
        else {
            GatewayRunner::ViewLease* view=gateway_->acquireView();
            if (!view) writeResponse(clientFd,503,"application/json","{\"ok\":false,\"error\":\"gateway unavailable\"}");
            else { std::string body=view->settingsJson(); delete view; writeResponse(clientFd,200,"application/json",body); }
        }
    } else if (request.find("GET /api/companion/config ") == 0) {
        writeResponse(clientFd, 200, "application/json", companionConfig_);
    } else if (automations_ != 0 && request.find("GET /api/automations ") == 0) {
        writeResponse(clientFd, 200, "application/json", automations_->listJson());
    } else if (automations_ != 0 && request.find("GET /api/automations/logs ") == 0) {
        writeResponse(clientFd, 200, "application/json", automations_->logsJson(""));
    } else if (automations_ != 0 && request.find("GET /api/automations/") == 0) {
        const std::string prefix = "GET /api/automations/";
        const std::string::size_type end = request.find(' ', prefix.size());
        const std::string filename = end == std::string::npos ? "" : decodeComponent(request.substr(prefix.size(), end-prefix.size()));
        std::string source, error;
        if (!automations_->source(filename, &source, &error)) writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
        else writeResponse(clientFd, 200, "application/json", "{\"filename\":\"" + jsonEscape(filename) + "\",\"source\":\"" + jsonEscape(source) + "\"}");
    } else if (automations_ != 0 && request.find("POST /api/automations/") == 0) {
        const std::string prefix = "POST /api/automations/";
        const std::string::size_type end = request.find(' ', prefix.size());
        const std::string operation = end == std::string::npos ? "" : request.substr(prefix.size(), end-prefix.size());
        const std::string::size_type bodyAt = request.find("\r\n\r\n");
        const std::string body = bodyAt == std::string::npos ? "" : request.substr(bodyAt+4);
        std::string filename, source, error;
        const bool hasFilename = formValue(body, "filename", &filename);
        bool result = false;
        if (operation == "validate" && hasFilename && formValue(body, "source", &source)) result = automations_->validate(filename, source, &error);
        else if (operation == "save" && hasFilename && formValue(body, "source", &source)) { std::string reload; result = automations_->save(filename, source, !formValue(body, "reload", &reload) || reload != "0", &error); }
        else if (operation == "reload" && (!hasFilename || filename == "all")) result = automations_->reload("", &error);
        else if (operation == "reload" && hasFilename) result = automations_->reload(filename, &error);
        else if (operation == "enable" && hasFilename) result = automations_->setEnabled(filename, true, &error);
        else if (operation == "disable" && hasFilename) result = automations_->setEnabled(filename, false, &error);
        else if (operation == "delete" && hasFilename) result = automations_->remove(filename, &error);
        else if (operation == "reset" && hasFilename) result = automations_->resetPersistent(filename, &error);
        else error = "invalid automation request";
        if (result) writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
    } else if (request.find("GET /api/status ") == 0) {
        GatewayRunner::ViewLease* view = gateway_->acquireView();
        if (view == 0) writeResponse(clientFd, 503, "application/json", "{\"ok\":false,\"error\":\"coordinator is unavailable\"}");
        else { std::string body = view->statusJson(); delete view; writeResponse(clientFd, 200, "application/json", body); }
    } else if (request.find("POST /api/devices/") == 0 && request.find("/actor ") != std::string::npos) {
        const std::string prefix = "POST /api/devices/";
        std::string::size_type suffix = request.find("/actor ", prefix.size());
        std::string address = suffix == std::string::npos ? "" : decodeComponent(request.substr(prefix.size(), suffix - prefix.size()));
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string state, error;
        bool valid = bodyAt != std::string::npos && formValue(request.substr(bodyAt + 4), "state", &state) && (state == "on" || state == "off");
        if (valid && gateway_->setActorState(address, state == "on", &error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(valid ? error : "invalid actor command") + "\"}");
    } else if (request.find("GET /api/devices/") == 0) {
        const std::string prefix = "GET /api/devices/";
        std::string::size_type end = request.find(' ', prefix.size());
        std::string target = end == std::string::npos ? "" : request.substr(prefix.size(), end - prefix.size());
        std::string::size_type suffix = target.find("/history");
        bool valid = suffix != std::string::npos && suffix > 0;
        std::string address = valid ? decodeComponent(target.substr(0, suffix)) : "";
        std::string query = valid ? target.substr(suffix + 8) : "";
        uint64_t before = 0;
        uint32_t since = 0;
        bool sawBefore = false, sawSince = false;
        valid = valid && (query.empty() || query[0] == '?');
        std::string::size_type at = 1;
        if (query == "?") valid = false;
        while (valid && at < query.size()) {
            const std::string::size_type amp = query.find('&', at);
            const std::string field = query.substr(at, amp == std::string::npos ? amp : amp - at);
            const std::string::size_type equal = field.find('=');
            const std::string key = field.substr(0, equal);
            const std::string number = equal == std::string::npos ? "" : field.substr(equal + 1);
            valid = !number.empty() && number.find_first_not_of("0123456789") == std::string::npos;
            char* parseEnd = 0;
            errno = 0;
            const uint64_t value = valid ? strtoull(number.c_str(), &parseEnd, 10) : 0;
            valid = valid && errno == 0 && *parseEnd == 0;
            if (key == "before") {
                valid = valid && !sawBefore && value > 0 && value < (static_cast<uint64_t>(1) << 50);
                before = value; sawBefore = true;
            } else if (key == "since") {
                valid = valid && !sawSince && value <= 0xffffffffUL;
                since = static_cast<uint32_t>(value); sawSince = true;
            } else valid = false;
            if (amp == std::string::npos) break;
            at = amp + 1;
            if (at == query.size()) valid = false;
        }
        std::string json;
        std::string error;
        if (!valid || !gateway_->readHistoryJson(address, before, &json, &error, since))
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
    } else if (request.find("GET /api/logs ") == 0 || request.find("GET /api/logs?") == 0) {
        std::string body;
        std::string::size_type targetStart = request.find(' ') + 1;
        std::string::size_type targetEnd = request.find(' ', targetStart);
        std::string target = request.substr(targetStart, targetEnd - targetStart);
        std::string::size_type query = target.find('?');
        std::string sinceText;
        unsigned long since = 0;
        bool hasSince = query != std::string::npos && formValue(target.substr(query + 1), "since", &sinceText);
        bool validSince = !hasSince || parseLogTimestamp(sinceText, &since);
        std::string limitText, beforeText;
        bool hasLimit = query != std::string::npos && formValue(target.substr(query + 1), "limit", &limitText);
        bool hasBefore = query != std::string::npos && formValue(target.substr(query + 1), "before", &beforeText);
        unsigned long limit = 0, before = 0;
        bool validPage = !hasLimit || (parseLogTimestamp(limitText, &limit) && limit > 0 && limit <= 500);
        validPage = validPage && (!hasBefore || (hasLimit && parseLogTimestamp(beforeText, &before) && before > 0 && before <= 0xffffffffUL));
#ifdef MICROHAD_TESTING
        if (!validSince || !validPage) {
            writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid log request\"}");
        } else if (hasSince && since > static_cast<unsigned long>(time(0))) {
            writeResponse(clientFd, 200, "application/json", "{\"lines\":[]}");
        } else {
            body = gateway_->logsJson();
            if (hasLimit) body.insert(body.size() - 1, ",\"next_before\":0,\"has_more\":false");
            writeResponse(clientFd, 200, "application/json", body);
        }
#else
        if (!validSince || !validPage) writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid log request\"}");
        else if (hasLimit) {
            if (SyslogLogReader::readPage(limit, before, &body))
                writeResponse(clientFd, 200, "application/json", body);
            else writeResponse(clientFd, 503, "application/json", "{\"ok\":false,\"error\":\"log is unavailable\"}");
        } else if ((hasSince ? SyslogLogReader::readJson(since, &body) : SyslogLogReader::readJson(&body)))
            writeResponse(clientFd, 200, "application/json", body);
        else writeResponse(clientFd, 503, "application/json", "{\"ok\":false,\"error\":\"log is unavailable\"}");
#endif
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
    } else if (request.find("POST /api/push/devices/register ") == 0) {
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string id, model, token, error;
        bool valid = bodyAt != std::string::npos &&
            jsonStringField(request.substr(bodyAt + 4), "installation_id", &id) &&
            jsonStringField(request.substr(bodyAt + 4), "model", &model) &&
            jsonStringField(request.substr(bodyAt + 4), "token", &token);
        if (valid && notifications_ != 0 && notifications_->registerDevice(id, model, token, &error))
            writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(valid ? error : "invalid push device registration") + "\"}");
    } else if (request.find("GET /api/push/devices ") == 0) {
        if (notifications_ != 0) writeResponse(clientFd, 200, "application/json", notifications_->devicesJson());
        else writeResponse(clientFd, 503, "application/json", "{\"ok\":false,\"error\":\"notification worker is unavailable\"}");
    } else if (request.find("POST /api/push/devices/") == 0) {
        const std::string prefix = "POST /api/push/devices/";
        std::string::size_type end = request.find(' ', prefix.size());
        std::string id = end == std::string::npos ? "" : request.substr(prefix.size(), end - prefix.size());
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string label, enabledText, presenceSensorId, error;
        bool deleting = id.size() > 7 && id.compare(id.size() - 7, 7, "/delete") == 0;
        if (deleting) id.erase(id.size() - 7);
        bool valid = deleting || (bodyAt != std::string::npos && formValue(request.substr(bodyAt + 4), "label", &label) &&
                     formValue(request.substr(bodyAt + 4), "enabled", &enabledText) &&
                     (enabledText == "true" || enabledText == "false"));
        bool hasPresenceSensor = !deleting && bodyAt != std::string::npos && formValue(request.substr(bodyAt + 4), "presence_sensor_id", &presenceSensorId);
        if (valid && notifications_ != 0 && (deleting ? notifications_->removeDevice(id, &error) :
            (hasPresenceSensor ? notifications_->updateDevice(id, label, enabledText == "true", presenceSensorId, &error) :
                                 notifications_->updateDevice(id, label, enabledText == "true", &error))))
            writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(valid ? error : "invalid push device settings") + "\"}");
    } else if (request.find("GET /api/push/browser-clients ") == 0) {
        std::string body = webPush_->clientsJson();
        writeResponse(clientFd, 200, "application/json", body);
    } else if (request.find("POST /api/push/browser-clients/") == 0) {
        const std::string prefix = "POST /api/push/browser-clients/";
        std::string::size_type end = request.find(' ', prefix.size());
        std::string id = end == std::string::npos ? "" : request.substr(prefix.size(), end - prefix.size());
        bool deleting = id.size() > 7 && id.compare(id.size() - 7, 7, "/delete") == 0;
        if (deleting) id.erase(id.size() - 7);
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string label, enabledText, presenceSensorId, error;
        bool valid = deleting || (bodyAt != std::string::npos && formValue(request.substr(bodyAt + 4), "label", &label) &&
                     formValue(request.substr(bodyAt + 4), "enabled", &enabledText) &&
                     (enabledText == "true" || enabledText == "false"));
        bool hasPresenceSensor = !deleting && bodyAt != std::string::npos && formValue(request.substr(bodyAt + 4), "presence_sensor_id", &presenceSensorId);
        bool changed = valid && (deleting ? webPush_->removeClient(id, &error) :
            (hasPresenceSensor ? webPush_->updateClient(id, label, enabledText == "true", presenceSensorId, &error) :
                                 webPush_->updateClient(id, label, enabledText == "true", &error)));
        if (changed) writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(valid ? error : "invalid browser client settings") + "\"}");
    } else if (request.find("GET /api/push/vapid-key ") == 0) {
        writeResponse(clientFd, 200, "application/json", "{\"public_key\":\"" + webPush_->publicKey() + "\"}");
    } else if (request.find("POST /api/push/subscriptions ") == 0) {
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string error;
        bool subscribed = bodyAt != std::string::npos && webPush_->addSubscription(request.substr(bodyAt + 4), &error);
        if (subscribed)
            writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + error + "\"}");
    } else if (request.find("POST /api/network/backup ") == 0) {
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string id, error;
        if (bodyAt == std::string::npos || !formValue(request.substr(bodyAt + 4), "id", &id))
            writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"missing backup id\"}");
        else if (gateway_->exportNetworkBackup(id, &error))
            writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"state\":\"exporting\"}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
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
        std::string::size_type reconfigureAt = request.find("/reconfigure ", prefix.size());
        std::string::size_type identifyAt = request.find("/identify ", prefix.size());
        std::string::size_type configurationAt = request.find("/configuration ", prefix.size());
        std::string::size_type thermostatAt = request.find("/thermostat ", prefix.size());
        std::string::size_type lightAt = request.find("/light ", prefix.size());
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        if (thermostatAt != std::string::npos) {
            std::string address = decodeComponent(request.substr(prefix.size(), thermostatAt - prefix.size()));
            std::string field, value, error, matterRequest;
            const std::string body = bodyAt == std::string::npos ? "" : request.substr(bodyAt + 4);
            bool valid = formValue(body, "field", &field) && formValue(body, "value", &value);
            if (valid && gateway_->setThermostat(address, field, value, &error, &matterRequest)) {
                uint32_t job=0;
                if (!matterRequest.empty() && (!matterWorker_ || !matterWorker_->submit(11,matterRequest,&job,&error))) {
                    std::string node; long endpoint=0; jsonStringField(matterRequest,"node",&node); jsonLongField(matterRequest,"endpoint",&endpoint);
                    gateway_->thermostatResult(node,static_cast<unsigned short>(endpoint),field,error.empty()?"Matter worker unavailable":error);
                    writeResponse(clientFd,400,"application/json","{\"ok\":false,\"error\":\"Matter thermostat request failed\"}");
                } else {
                    std::ostringstream response; response<<"{\"ok\":true,\"job\":"<<job<<",\"message\":\"Thermostat request queued; awaiting device confirmation.\"}";
                    writeResponse(clientFd,200,"application/json",response.str());
                }
            }
            else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(valid ? error : "invalid thermostat request") + "\"}");
        } else if (configurationAt != std::string::npos) {
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
        } else if (reconfigureAt != std::string::npos) {
            std::string ieeeAddress = decodeComponent(request.substr(prefix.size(), reconfigureAt - prefix.size()));
            std::string error;
            if (gateway_->reconfigureDevice(ieeeAddress, &error))
                writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"message\":\"Device reconfiguration queued.\"}");
            else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
        } else if (refreshAt != std::string::npos) {
            std::string ieeeAddress = decodeComponent(request.substr(prefix.size(), refreshAt - prefix.size()));
            std::string error;
            if (gateway_->refreshDevice(ieeeAddress, &error))
                writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"message\":\"Device refresh queued.\"}");
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
            std::string triggerModeValue;
            std::string triggerCooldownValue;
            std::string error;
            bool hasName = formValue(body, "name", &name);
            formValue(body, "low", &lowValue);
            formValue(body, "high", &highValue);
            bool hasInitial = formValue(body, "initial_minutes", &initialValue);
            bool hasReminder = formValue(body, "reminder_minutes", &reminderValue);
            bool hasSensorRefresh = formValue(body, "sensor_refresh_minutes", &sensorRefreshValue);
            bool hasTriggerMode = formValue(body, "contact_trigger_mode", &triggerModeValue);
            bool hasTriggerCooldown = formValue(body, "contact_trigger_cooldown_seconds", &triggerCooldownValue);
            std::map<std::string, DeviceValueAlert> valueAlerts;
            static const char* alertNames[] = {"temperature", "humidity", "battery", "link_quality", "wifi_signal", "brightness", "power", "co2", "pm25"};
            bool validAlerts = true;
            for (unsigned int alertIndex = 0; alertIndex < 9; ++alertIndex) {
                std::string prefix = std::string("value_alert_") + alertNames[alertIndex] + "_";
                std::string lowLimit, highLimit, hysteresisValue;
                bool lowProvided = formValue(body, (prefix + "low").c_str(), &lowLimit);
                bool highProvided = formValue(body, (prefix + "high").c_str(), &highLimit);
                bool marginProvided = formValue(body, (prefix + "hysteresis").c_str(), &hysteresisValue);
                if (!lowProvided && !highProvided && !marginProvided) continue;
                DeviceValueAlert alert;
                if (alertIndex >= 7) formValue(body, (prefix + "unit").c_str(), &alert.unit);
                alert.hasLow = !lowLimit.empty(); alert.hasHigh = !highLimit.empty();
                if ((alert.hasLow && !parseDecimal(lowLimit, &alert.low)) ||
                    (alert.hasHigh && !parseDecimal(highLimit, &alert.high)) ||
                    (marginProvided && !hysteresisValue.empty() && !parseDecimal(hysteresisValue, &alert.hysteresis)) ||
                    (!hysteresisValue.empty() && alert.hysteresis < 0)) validAlerts = false;
                valueAlerts[alertNames[alertIndex]] = alert;
            }
            bool hasLow = !lowValue.empty();
            bool hasHigh = !highValue.empty();
            double low = 0;
            double high = 0;
            unsigned long initial = 0;
            unsigned long reminder = 0;
            unsigned long sensorRefresh = 0;
            unsigned long triggerCooldown = 60;
            bool triggerMode = false;
            bool validTrigger = hasTriggerMode == hasTriggerCooldown;
            if (hasTriggerMode) {
                validTrigger = validTrigger && (triggerModeValue == "true" || triggerModeValue == "false") &&
                               parseMinutes(triggerCooldownValue, &triggerCooldown) && triggerCooldown != 0;
                triggerMode = triggerModeValue == "true";
            }
            if (!hasName || !validAlerts || (hasLow && !parseDecimal(lowValue, &low)) || (hasHigh && !parseDecimal(highValue, &high)) ||
                (hasInitial && !parseMinutes(initialValue, &initial)) ||
                (hasReminder && !parseMinutes(reminderValue, &reminder)) ||
                (hasSensorRefresh && !parseMinutes(sensorRefreshValue, &sensorRefresh)) || !validTrigger) {
                writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid device settings\"}");
            } else if (gateway_->setDeviceSettings(ieeeAddress, name, hasLow, low, hasHigh, high,
                                                    hasInitial && hasReminder, initial, reminder, hasSensorRefresh,
                                                    sensorRefresh, &error, hasTriggerMode, triggerMode, triggerCooldown) &&
                       (valueAlerts.empty() || gateway_->setDeviceValueAlerts(ieeeAddress, valueAlerts, &error))) {
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
            std::string filename = webuiDirectory_ + path + ".gz";
            struct stat info;
            std::string body;
            if (lstat(filename.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
                readTextFile(filename, &body))
                writeResponse(clientFd, 200, contentType, body, "gzip");
            else if (path == "/index.html")
                writeResponse(clientFd, 500, "text/plain; charset=utf-8", "Dashboard is not installed.");
            else writeResponse(clientFd, 404, "text/plain; charset=utf-8", "Asset not found.");
        }
    } else {
        writeResponse(clientFd, 400, "application/json", "{\"error\":\"unknown endpoint\"}");
    }
}

std::string HttpServer::controllerStatsJson() {
    double uptime = 0;
    double cpuUsage = -1;
    uint64_t totalTicks = 0, idleTicks = 0, startTicks = 0;
    uint64_t processPages = 0, zigguratMemory = 0, matterMemory = 0;
    struct statvfs filesystem;
    uint64_t storageTotal = 0, storageFree = 0;
    double load1 = 0, load5 = 0, load15 = 0;
    bool hasUptime = readUptime(&uptime);
    bool hasCpu = readCpuCounters(&totalTicks, &idleTicks);
    MemoryStats memory = readMemory();
    bool hasMemory = memory.hasTotal, hasFree = memory.hasFree;
    uint64_t memoryTotal = memory.total * 1024, memoryFree = memory.free * 1024;
    bool hasReclaimable = memory.hasReclaimable || memory.hasCached || memory.hasSlab;
    uint64_t reclaimableKb = memory.hasReclaimable ? memory.reclaimable : memory.cached + memory.slab;
    if (hasMemory && hasFree && memoryFree > memoryTotal) memoryFree = memoryTotal;
    ProcessStat self;
    char selfPath[96]; snprintf(selfPath, sizeof(selfPath), "/proc/%lu/stat", processId_);
    bool hasStart = readProcessStat(selfPath, &self);
    startTicks = self.startTicks;
    long pageSize = sysconf(_SC_PAGESIZE);
    bool hasProcessMemory = hasStart && pageSize > 0;
    if (hasProcessMemory) processPages = self.rssPages * static_cast<uint64_t>(pageSize);
    bool hasFilesystem = statvfs(gateway_->runtimeDirectory().c_str(), &filesystem) == 0;
    if (hasFilesystem) {
        storageTotal = static_cast<uint64_t>(filesystem.f_blocks) * filesystem.f_frsize;
        storageFree = static_cast<uint64_t>(filesystem.f_bavail) * filesystem.f_frsize;
    }
    char load[128];
    bool hasLoad = readProcLine("/proc/loadavg", load, sizeof(load)) &&
        sscanf(load, "%lf %lf %lf", &load1, &load5, &load15) == 3;
    long ticksPerSecond = sysconf(_SC_CLK_TCK);
    double serviceUptime = hasUptime && hasStart && ticksPerSecond > 0 ? uptime - static_cast<double>(startTicks) / ticksPerSecond : -1;
    std::map<uint64_t, ThreadCpuReading> readings;
    bool hasZigguratMemory = false, hasMatterMemory = false;
    if (hasStart) collectProcesses(processId_, self, pageSize, &readings,
        &zigguratMemory, &hasZigguratMemory, &matterMemory, &hasMatterMemory);
    std::vector<ThreadCpuReading> threads;
    std::map<uint64_t, ThreadCpuSample> currentSamples;
    pthread_mutex_lock(&statsMutex_);
    if (hasCpu && (!hasUptime || uptime >= previousThreadSampleUptime_)) {
        if (hasPreviousCpu_ && totalTicks > previousCpuTotal_) {
            uint64_t totalDelta = totalTicks - previousCpuTotal_;
            uint64_t idleDelta = idleTicks >= previousCpuIdle_ ? idleTicks - previousCpuIdle_ : 0;
            cpuUsage = totalDelta ? 100.0 * (totalDelta - (idleDelta > totalDelta ? totalDelta : idleDelta)) / totalDelta : 0;
        }
        previousCpuTotal_ = totalTicks; previousCpuIdle_ = idleTicks; hasPreviousCpu_ = true;
    }
    for (std::map<uint64_t, ThreadCpuReading>::iterator it = readings.begin(); it != readings.end(); ++it) {
        ThreadCpuReading& reading = it->second;
        std::map<uint64_t, ThreadCpuSample>::const_iterator previous = previousThreadSamples_.find(reading.tid);
        if (previous != previousThreadSamples_.end() && previous->second.startTicks == reading.startTicks &&
            reading.cpuTicks >= previous->second.cpuTicks && hasUptime && previousThreadSampleUptime_ > 0 &&
            uptime > previousThreadSampleUptime_ && ticksPerSecond > 0) {
            double elapsedTicks = (uptime - previousThreadSampleUptime_) * ticksPerSecond;
            reading.usage = 100.0 * (reading.cpuTicks - previous->second.cpuTicks) / elapsedTicks;
            reading.sampled = true;
        }
        ThreadCpuSample sample; sample.startTicks = reading.startTicks; sample.cpuTicks = reading.cpuTicks;
        currentSamples[reading.tid] = sample;
        threads.push_back(reading);
    }
    // An overlapping request can finish sampling before an older request does.
    // Never replace the baseline with counters from an earlier observation.
    if (hasUptime && uptime > previousThreadSampleUptime_) {
        previousThreadSamples_.swap(currentSamples);
        previousThreadSampleUptime_ = uptime;
    }
    pthread_mutex_unlock(&statsMutex_);
    std::sort(threads.begin(), threads.end(), threadUsageOrder);
    long serialOpenedAt = 0;
    GatewayRunner::ViewLease* view = gateway_->acquireView();
    if (view != 0) { serialOpenedAt = view->serialOpenedAt(); delete view; }
    JsonWriter json;
    json << "{\"build_date\":\"" << __DATE__ << " " << __TIME__
         << "\",\"build_commit\":\"" << MICROHAD_BUILD_REVISION << "\",\"device_uptime_seconds\":";
    if (hasUptime) json << static_cast<unsigned long>(uptime); else json << "null";
    json << ",\"service_uptime_seconds\":";
    if (serviceUptime >= 0) json << static_cast<unsigned long>(serviceUptime); else json << "null";
    json << ",\"serial_opened_at\":" << (serialOpenedAt ? serialOpenedAt : 0) << ",\"serial_uptime_seconds\":";
    if (serialOpenedAt && time(0) >= serialOpenedAt) json << static_cast<unsigned long>(time(0) - serialOpenedAt); else json << "null";
    json << ",\"cpu_usage_percent\":";
    if (cpuUsage >= 0) json << cpuUsage; else json << "null";
    json << ",\"cpu_load_1\":"; if (hasLoad) json << load1; else json << "null";
    json << ",\"cpu_load_5\":"; if (hasLoad) json << load5; else json << "null";
    json << ",\"cpu_load_15\":"; if (hasLoad) json << load15; else json << "null";
    json << ",\"memory_total_bytes\":"; if (hasMemory) json << memoryTotal; else json << "null";
    json << ",\"memory_free_bytes\":"; if (hasFree) json << memoryFree; else json << "null";
    json << ",\"memory_reclaimable_bytes\":"; if (hasReclaimable) json << reclaimableKb * 1024; else json << "null";
    json << ",\"process_memory_bytes\":"; if (hasProcessMemory && pageSize > 0) json << processPages; else json << "null";
    json << ",\"ziggurat_memory_bytes\":"; if (hasZigguratMemory) json << zigguratMemory; else json << "null";
    json << ",\"matter_memory_bytes\":"; if (hasMatterMemory) json << matterMemory; else json << "null";
    json << ",\"syslog_ring_bytes\":" << 512 * 1024
         << ",\"values_file_bytes\":" << fileBytes(gateway_->historyPath())
         << ",\"storage_total_bytes\":"; if (hasFilesystem) json << storageTotal; else json << "null";
    json << ",\"storage_free_bytes\":"; if (hasFilesystem) json << storageFree; else json << "null";
    json << ",\"storage_used_bytes\":"; if (hasFilesystem) json << (storageTotal >= storageFree ? storageTotal - storageFree : 0); else json << "null";
    json << ",\"threads\":[";
    for (unsigned long i = 0; i < threads.size(); ++i) {
        if (i) json << ',';
        json << "{\"tid\":" << threads[i].tid << ",\"process\":\"" << jsonEscape(threads[i].process)
             << "\",\"name\":\"" << jsonEscape(threads[i].name)
             << "\",\"cpu_usage_percent\":";
        if (threads[i].sampled) json << threads[i].usage; else json << "null";
        json << '}';
    }
    json << "]}";
    return json.str();
}
