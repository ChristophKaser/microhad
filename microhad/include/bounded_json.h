#ifndef MICROHAD_BOUNDED_JSON_H
#define MICROHAD_BOUNDED_JSON_H
#include <map>
#include <string>
#include <vector>
typedef std::map<std::string,std::string> JsonFields;
class BoundedJson {
public:
    explicit BoundedJson(const std::string& text) : text_(text), at_(0) {}
    bool parse(JsonFields* fields) { return object(fields, 0) && whitespace() == text_.size(); }
    bool parseArray(std::vector<std::string>* values) { return array(values, 0) && whitespace() == text_.size(); }
private:
    const std::string& text_;
    unsigned long at_;
    unsigned long whitespace() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\n' ||
               text_[at_] == '\r' || text_[at_] == '\t')) ++at_;
        return at_;
    }
    bool take(char c) {
        whitespace();
        if (at_ >= text_.size() || text_[at_] != c) return false;
        ++at_; return true;
    }
    bool string(std::string* value) {
        if (!take('"')) return false;
        value->clear();
        while (at_ < text_.size() && text_[at_] != '"') {
            unsigned char c = text_[at_++];
            if (c < 0x20 || value->size() >= 1536) return false;
            if (c == '\\') {
                if (at_ >= text_.size()) return false;
                char escaped=text_[at_++];
                if (escaped!='\"' && escaped!='\\' && escaped!='/' && escaped!='n' && escaped!='r' && escaped!='t' && escaped!='b' && escaped!='f') return false;
                *value += '\\'; *value += escaped; continue;
            }
            *value += static_cast<char>(c);
        }
        return take('"');
    }
    bool array(std::vector<std::string>* values, unsigned int depth) {
        if (depth > 4 || !take('[')) return false;
        whitespace();
        if (at_ < text_.size() && text_[at_] == ']') { ++at_; return true; }
        while (values->size() < 128) {
            unsigned long start = whitespace();
            if (at_ >= text_.size()) return false;
            if (text_[at_] == '{') {
                JsonFields nested;
                if (!object(&nested, depth + 1)) return false;
            } else {
                while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
                if (at_ == start || at_ - start > 10 || (at_ - start > 1 && text_[start] == '0')) return false;
            }
            values->push_back(text_.substr(start, at_ - start));
            whitespace();
            if (at_ < text_.size() && text_[at_] == ']') { ++at_; return true; }
            if (!take(',')) return false;
        }
        return false;
    }
    bool object(JsonFields* fields, unsigned int depth) {
        if (depth > 4 || !take('{')) return false;
        whitespace();
        if (at_ < text_.size() && text_[at_] == '}') { ++at_; return true; }
        while (fields->size() < 12) {
            std::string key, value;
            if (!string(&key) || fields->count(key) || !take(':')) return false;
            whitespace();
            if (at_ >= text_.size()) return false;
            if (text_[at_] == '{') {
                unsigned long start = at_;
                JsonFields nested;
                if (!object(&nested, depth + 1)) return false;
                value = text_.substr(start, at_ - start);
            } else if (text_[at_] == '[') {
                unsigned long start = at_;
                std::vector<std::string> values;
                if (!array(&values, depth + 1)) return false;
                value = text_.substr(start, at_ - start);
            } else if (text_[at_] == '"') {
                if (!string(&value)) return false;
                value = '"' + value + '"';
            } else {
                unsigned long start = at_;
                while (at_ < text_.size() && text_[at_] != ',' && text_[at_] != '}' &&
                       text_[at_] != ' ' && text_[at_] != '\t' && text_[at_] != '\r' && text_[at_] != '\n') ++at_;
                value = text_.substr(start, at_ - start);
                if (value != "true" && value != "false" && value != "null" && !jsonNumber(value)) return false;
            }
            (*fields)[key] = value;
            whitespace();
            if (at_ < text_.size() && text_[at_] == '}') { ++at_; return true; }
            if (!take(',')) return false;
        }
        return false;
    }
    static bool jsonNumber(const std::string& value) {
        if (value.empty() || value.size() > 32) return false;
        unsigned long i = value[0] == '-' ? 1 : 0;
        if (i == value.size()) return false;
        if (value[i] == '0') ++i;
        else {
            if (value[i] < '1' || value[i] > '9') return false;
            while (i < value.size() && value[i] >= '0' && value[i] <= '9') ++i;
        }
        if (i < value.size() && value[i] == '.') {
            ++i; unsigned long start = i;
            while (i < value.size() && value[i] >= '0' && value[i] <= '9') ++i;
            if (i == start) return false;
        }
        if (i < value.size() && (value[i] == 'e' || value[i] == 'E')) {
            ++i; if (i < value.size() && (value[i] == '+' || value[i] == '-')) ++i;
            unsigned long start = i;
            while (i < value.size() && value[i] >= '0' && value[i] <= '9') ++i;
            if (i == start) return false;
        }
        return i == value.size();
    }
};
#endif
