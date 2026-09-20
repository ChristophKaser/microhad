#include "fcm.h"
#include "storage.h"

namespace {
std::string jsonValue(const std::string& json, const char* name) {
    std::string marker = std::string("\"") + name + "\"";
    std::string::size_type nameAt = json.find(marker);
    std::string::size_type start;
    std::string::size_type end;
    std::string result;
    bool escaped = false;
    if (nameAt == std::string::npos) return result;
    start = json.find('"', json.find(':', nameAt + marker.size()) + 1);
    if (start == std::string::npos) return result;
    ++start;
    for (end = start; end < json.size(); ++end) {
        if (escaped) {
            if (json[end] == 'n') result += '\n'; else result += json[end];
            escaped = false;
        } else if (json[end] == '\\') escaped = true;
        else if (json[end] == '"') return result;
        else result += json[end];
    }
    return std::string();
}
}

bool parseFirebaseServiceAccount(const std::string& json, FirebaseServiceAccount* account, std::string* error) {
    account->projectId = jsonValue(json, "project_id");
    account->clientEmail = jsonValue(json, "client_email");
    account->privateKeyPem = jsonValue(json, "private_key");
    if (account->projectId.empty() || account->clientEmail.empty() || account->privateKeyPem.empty()) {
        *error = "Firebase service account is missing project_id, client_email, or private_key";
        return false;
    }
    return true;
}

std::string base64UrlEncode(const std::string& data) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    unsigned long i;
    for (i = 0; i < data.size(); i += 3) {
        unsigned int bits = static_cast<unsigned char>(data[i]) << 16;
        unsigned int remaining = static_cast<unsigned int>(data.size() - i);
        if (remaining > 1) bits |= static_cast<unsigned char>(data[i + 1]) << 8;
        if (remaining > 2) bits |= static_cast<unsigned char>(data[i + 2]);
        output += alphabet[(bits >> 18) & 63];
        output += alphabet[(bits >> 12) & 63];
        if (remaining > 1) output += alphabet[(bits >> 6) & 63];
        if (remaining > 2) output += alphabet[bits & 63];
    }
    return output;
}

std::string makeFcmMessage(const std::string& topic, const std::string& title, const std::string& body) {
    return std::string("{\"message\":{\"topic\":\"") + jsonEscape(topic) + "\",\"notification\":{\"title\":\"" +
           jsonEscape(title) + "\",\"body\":\"" + jsonEscape(body) + "\"},\"android\":{\"priority\":\"high\"}}}";
}
