#include "json_util.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>

namespace {
bool fieldValueStart(const std::string& input, const std::string& name, std::string::size_type* start) {
    const std::string marker = "\"" + name + "\"";
    std::string::size_type at = input.find(marker);
    if (at == std::string::npos) return false;
    at = input.find(':', at + marker.size());
    if (at == std::string::npos) return false;
    *start = at + 1;
    while (*start < input.size() && (input[*start] == ' ' || input[*start] == '\t' || input[*start] == '\r' || input[*start] == '\n')) ++*start;
    return *start < input.size();
}
}

bool jsonStringField(const std::string& input, const std::string& name, std::string* value) {
    std::string::size_type at;
    bool escaped = false;
    value->clear();
    if (!fieldValueStart(input, name, &at) || input[at] != '\"') return false;
    for (++at; at < input.size(); ++at) {
        char current = input[at];
        if (escaped) {
            if (current == 'n') value->push_back('\n');
            else if (current == 'r') value->push_back('\r');
            else if (current == 't') value->push_back('\t');
            else value->push_back(current);
            escaped = false;
        } else if (current == '\\') escaped = true;
        else if (current == '\"') return true;
        else value->push_back(current);
    }
    value->clear();
    return false;
}

bool jsonLongField(const std::string& input, const std::string& name, long* value) {
    std::string::size_type at;
    char* end = 0;
    if (!fieldValueStart(input, name, &at)) return false;
    errno = 0;
    *value = strtol(input.c_str() + at, &end, 10);
    return errno == 0 && end != input.c_str() + at;
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    std::string::const_iterator it;
    for (it = value.begin(); it != value.end(); ++it) {
        if (*it == '\\' || *it == '\"') result += '\\';
        result += *it;
    }
    return result;
}

bool readTextFile(const std::string& filename, std::string* content) {
    std::ifstream input(filename.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    content->assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return true;
}
