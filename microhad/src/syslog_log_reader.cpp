#include "syslog_log_reader.h"
#include "json_util.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
const unsigned long RingBytes = 512UL * 1024UL;

bool collectSyslogLines(std::vector<std::string>* lines) {
    int descriptors[2];
    if (pipe(descriptors) != 0) return false;
    pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]); close(descriptors[1]); return false;
    }
    if (child == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(126);
        close(descriptors[1]);
        const char* executable = getenv("MICROHAD_LOGREAD_PATH");
        if (executable == 0 || *executable == '\0') executable = "/tuya/native/logread";
        execl(executable, "logread", static_cast<char*>(0));
        _exit(127);
    }
    close(descriptors[1]);
    std::string output;
    char chunk[2048];
    bool valid = true;
    for (;;) {
        ssize_t count = read(descriptors[0], chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { valid = false; break; }
        if (count == 0) break;
        output.append(chunk, static_cast<std::string::size_type>(count));
        if (output.size() > RingBytes + 4096UL) { valid = false; break; }
    }
    close(descriptors[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    if (!valid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    std::string::size_type begin = 0;
    while (begin < output.size()) {
        std::string::size_type end = output.find('\n', begin);
        if (end == std::string::npos) end = output.size();
        if (end > begin) lines->push_back(output.substr(begin, end - begin));
        begin = end + 1;
    }
    return true;
}

long timestampOf(const std::string& line) {
    char month[4] = {0, 0, 0, 0};
    int day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(line.c_str(), "%3s %d %d:%d:%d", month, &day, &hour, &minute, &second) != 5) return 0;
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int monthIndex = -1;
    for (int i = 0; i < 12; ++i) if (strcmp(month, months[i]) == 0) monthIndex = i;
    if (monthIndex < 0 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return 0;
    time_t now = time(0);
    struct tm current;
    if (localtime_r(&now, &current) == 0) return 0;
    struct tm parsed = current;
    parsed.tm_mon = monthIndex; parsed.tm_mday = day;
    parsed.tm_hour = hour; parsed.tm_min = minute; parsed.tm_sec = second; parsed.tm_isdst = -1;
    time_t result = mktime(&parsed);
    if (result > now + 86400) { parsed.tm_year -= 1; result = mktime(&parsed); }
    return static_cast<long>(result);
}

std::string apiLine(const std::string& line) {
    long timestamp = timestampOf(line);
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%ld ", timestamp);
    return std::string(prefix) + line;
}

std::string linesJson(const std::vector<std::string>& lines, unsigned long begin, unsigned long end) {
    std::ostringstream json;
    json << "{\"lines\":[";
    for (unsigned long i = begin; i < end; ++i) {
        if (i != begin) json << ',';
        json << '\"' << jsonEscape(apiLine(lines[i])) << '\"';
    }
    json << "]}";
    return json.str();
}
}

bool SyslogLogReader::readJson(std::string* json) {
    std::vector<std::string> lines;
    if (json == 0 || !collectSyslogLines(&lines)) return false;
    *json = linesJson(lines, 0, static_cast<unsigned long>(lines.size()));
    return true;
}

bool SyslogLogReader::readJson(unsigned long since, std::string* json) {
    std::vector<std::string> lines;
    if (json == 0 || !collectSyslogLines(&lines)) return false;
    std::ostringstream result;
    result << "{\"lines\":[";
    bool first = true;
    for (unsigned long i = 0; i < lines.size(); ++i) {
        if (static_cast<unsigned long>(timestampOf(lines[i])) < since) continue;
        if (!first) result << ',';
        first = false;
        result << '\"' << jsonEscape(apiLine(lines[i])) << '\"';
    }
    result << "]}";
    *json = result.str();
    return true;
}

bool SyslogLogReader::readPage(unsigned long limit, unsigned long before, std::string* json) {
    std::vector<std::string> lines;
    if (json == 0 || limit == 0 || limit > 500 || !collectSyslogLines(&lines)) return false;
    unsigned long count = static_cast<unsigned long>(lines.size());
    unsigned long end = before == 0 || before > count ? count : before;
    unsigned long begin = end > limit ? end - limit : 0;
    *json = linesJson(lines, begin, end);
    if (begin == 0) {
        json->insert(json->size() - 1, ",\"next_before\":0,\"has_more\":false");
    } else {
        std::ostringstream fields;
        fields << ",\"next_before\":" << begin << ",\"has_more\":true";
        json->insert(json->size() - 1, fields.str());
    }
    return true;
}
