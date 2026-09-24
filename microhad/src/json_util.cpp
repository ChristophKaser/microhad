#include "json_util.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>

namespace {
bool isWhitespace(char value) { return value == ' ' || value == '\t' || value == '\r' || value == '\n'; }
void skipWhitespace(const std::string& input, std::string::size_type* at) { while (*at < input.size() && isWhitespace(input[*at])) ++*at; }
bool hexValue(char value, unsigned long* result) {
    if (value >= '0' && value <= '9') *result = value - '0';
    else if (value >= 'a' && value <= 'f') *result = value - 'a' + 10;
    else if (value >= 'A' && value <= 'F') *result = value - 'A' + 10;
    else return false;
    return true;
}
bool unicodeEscape(const std::string& input, std::string::size_type* at, unsigned long* codepoint) {
    unsigned long value = 0, digit, i;
    if (*at + 4 > input.size()) return false;
    for (i = 0; i < 4; ++i) { if (!hexValue(input[*at + i], &digit)) return false; value = (value << 4) | digit; }
    *at += 4; *codepoint = value; return true;
}
void appendUtf8(std::string* output, unsigned long value) {
    if (value <= 0x7f) output->push_back(static_cast<char>(value));
    else if (value <= 0x7ff) { output->push_back(static_cast<char>(0xc0 | (value >> 6))); output->push_back(static_cast<char>(0x80 | (value & 0x3f))); }
    else if (value <= 0xffff) { output->push_back(static_cast<char>(0xe0 | (value >> 12))); output->push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f))); output->push_back(static_cast<char>(0x80 | (value & 0x3f))); }
    else { output->push_back(static_cast<char>(0xf0 | (value >> 18))); output->push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f))); output->push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f))); output->push_back(static_cast<char>(0x80 | (value & 0x3f))); }
}
bool parseString(const std::string& input, std::string::size_type* at, std::string* value) {
    if (*at >= input.size() || input[*at] != '\"') return false;
    ++*at; value->clear();
    while (*at < input.size()) {
        unsigned char current = static_cast<unsigned char>(input[*at]); ++*at;
        if (current == '\"') return true;
        if (current < 0x20) return false;
        if (current != '\\') { value->push_back(static_cast<char>(current)); continue; }
        if (*at >= input.size()) return false;
        current = static_cast<unsigned char>(input[*at]); ++*at;
        if (current == '\"' || current == '\\' || current == '/') value->push_back(static_cast<char>(current));
        else if (current == 'b') value->push_back('\b'); else if (current == 'f') value->push_back('\f');
        else if (current == 'n') value->push_back('\n'); else if (current == 'r') value->push_back('\r'); else if (current == 't') value->push_back('\t');
        else if (current == 'u') {
            unsigned long codepoint, low;
            if (!unicodeEscape(input, at, &codepoint)) return false;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                if (*at + 2 > input.size() || input[*at] != '\\' || input[*at + 1] != 'u') return false;
                *at += 2;
                if (!unicodeEscape(input, at, &low) || low < 0xdc00 || low > 0xdfff) return false;
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
            } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) return false;
            appendUtf8(value, codepoint);
        } else return false;
    }
    value->clear(); return false;
}
bool fieldValueStart(const std::string& input, const std::string& name, std::string::size_type* start) {
    std::string::size_type at = 0; std::string field;
    std::string::size_type first = input.find_first_not_of(" \t\r\n");
    std::string::size_type last = input.find_last_not_of(" \t\r\n");
    bool found = false;
    if (first == std::string::npos || input[first] != '{' || input[last] != '}') return false;
    while (at < input.size()) {
        if (input[at] != '\"') { ++at; continue; }
        if (!parseString(input, &at, &field)) return false;
        skipWhitespace(input, &at);
        if (at < input.size() && input[at] == ':') {
            ++at; skipWhitespace(input, &at);
            if (field == name && at < input.size()) {
                if (found) return false;
                *start = at;
                found = true;
            }
        }
    }
    return found;
}
}

bool jsonStringField(const std::string& input, const std::string& name, std::string* value) {
    std::string::size_type at;
    return fieldValueStart(input, name, &at) && parseString(input, &at, value);
}

bool jsonLongField(const std::string& input, const std::string& name, long* value) {
    std::string::size_type at;
    char* end = 0;
    if (!fieldValueStart(input, name, &at)) return false;
    errno = 0;
    *value = strtol(input.c_str() + at, &end, 10);
    return errno == 0 && end != input.c_str() + at && (*end == 0 || *end == ',' || *end == '}' || *end == ']' || isWhitespace(*end));
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    std::string::const_iterator it;
    static const char digits[] = "0123456789abcdef";
    for (it = value.begin(); it != value.end(); ++it) {
        unsigned char current = static_cast<unsigned char>(*it);
        if (current == '\\') result += "\\\\";
        else if (current == '\"') result += "\\\"";
        else if (current == '\b') result += "\\b";
        else if (current == '\f') result += "\\f";
        else if (current == '\n') result += "\\n";
        else if (current == '\r') result += "\\r";
        else if (current == '\t') result += "\\t";
        else if (current < 0x20) { result += "\\u00"; result += digits[current >> 4]; result += digits[current & 0x0f]; }
        else result += static_cast<char>(current);
    }
    return result;
}

bool readTextFile(const std::string& filename, std::string* content) {
    std::ifstream input(filename.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    content->assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return true;
}
