#ifndef MICROHAD_JSON_WRITER_H
#define MICROHAD_JSON_WRITER_H

#include <cstdio>
#include <string>

// The default decimal/precision-six formatting used by JSON responses, without
// iostream sentries or per-insertion locale lookups. Strings must already be
// JSON-escaped, just as when inserting them into an ostringstream.
class JsonWriter {
public:
    JsonWriter() { buffer_.reserve(4096); }
    JsonWriter& operator<<(const char* value) { buffer_ += value; return *this; }
    JsonWriter& operator<<(const std::string& value) { buffer_ += value; return *this; }
    JsonWriter& operator<<(char value) { buffer_ += value; return *this; }
    JsonWriter& operator<<(signed char value) { buffer_ += static_cast<char>(value); return *this; }
    JsonWriter& operator<<(unsigned char value) { buffer_ += static_cast<char>(value); return *this; }
    JsonWriter& operator<<(int value) { char text[32]; snprintf(text, sizeof(text), "%d", value); return *this << text; }
    JsonWriter& operator<<(unsigned int value) { char text[32]; snprintf(text, sizeof(text), "%u", value); return *this << text; }
    JsonWriter& operator<<(long value) { char text[32]; snprintf(text, sizeof(text), "%ld", value); return *this << text; }
    JsonWriter& operator<<(unsigned long value) { char text[32]; snprintf(text, sizeof(text), "%lu", value); return *this << text; }
    JsonWriter& operator<<(long long value) { char text[32]; snprintf(text, sizeof(text), "%lld", value); return *this << text; }
    JsonWriter& operator<<(unsigned long long value) { char text[32]; snprintf(text, sizeof(text), "%llu", value); return *this << text; }
    JsonWriter& operator<<(double value) { char text[128]; snprintf(text, sizeof(text), "%.6g", value); return *this << text; }
    const std::string& str() const { return buffer_; }
private:
    std::string buffer_;
};

#endif
