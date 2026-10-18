#include "gateway_config.h"

#include "json_util.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits.h>
#include <map>

namespace {
void skipWhitespace(const std::string& input, std::string::size_type* at) {
    while (*at < input.size() && (input[*at] == ' ' || input[*at] == '\t' || input[*at] == '\r' || input[*at] == '\n')) ++*at;
}

bool scanString(const std::string& input, std::string::size_type* at, std::string* raw) {
    std::string::size_type start = *at;
    if (*at >= input.size() || input[*at] != '"') return false;
    ++*at;
    while (*at < input.size()) {
        char value = input[*at];
        ++*at;
        if (value == '"') { *raw = input.substr(start, *at - start); return true; }
        if (static_cast<unsigned char>(value) < 0x20) return false;
        if (value == '\\') {
            if (*at >= input.size()) return false;
            value = input[*at];
            ++*at;
            if (value == 'u') {
                unsigned int i;
                for (i = 0; i < 4; ++i) {
                    if (*at >= input.size()) return false;
                    char digit = input[*at];
                    if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') ||
                          (digit >= 'A' && digit <= 'F'))) return false;
                    ++*at;
                }
            } else if (value != '"' && value != '\\' && value != '/' && value != 'b' && value != 'f' &&
                       value != 'n' && value != 'r' && value != 't') return false;
        }
    }
    return false;
}

bool decodeJsonString(const std::string& raw, std::string* value) {
    return jsonStringField(std::string("{\"value\":") + raw + "}", "value", value);
}

bool parseObject(const std::string& input, std::map<std::string, std::string>* strings,
                 std::map<std::string, long>* numbers, std::string* error) {
    std::string::size_type at = 0;
    skipWhitespace(input, &at);
    if (at >= input.size() || input[at++] != '{') { *error = "configuration must be a JSON object"; return false; }
    skipWhitespace(input, &at);
    if (at < input.size() && input[at] == '}') {
        ++at; skipWhitespace(input, &at);
        if (at == input.size()) return true;
        *error = "unexpected data after configuration object"; return false;
    }
    while (at < input.size()) {
        std::string rawKey, key;
        if (!scanString(input, &at, &rawKey) || !decodeJsonString(rawKey, &key)) {
            *error = "configuration contains an invalid field name"; return false;
        }
        skipWhitespace(input, &at);
        if (at >= input.size() || input[at++] != ':') { *error = "configuration field is missing ':'"; return false; }
        skipWhitespace(input, &at);
        std::string::size_type start = at;
        if (at < input.size() && input[at] == '"') {
            std::string rawValue, value;
            if (!scanString(input, &at, &rawValue) || !decodeJsonString(rawValue, &value)) {
                *error = "configuration field has an invalid string"; return false;
            }
            if (strings->count(key) || numbers->count(key)) { *error = "duplicate configuration field: " + key; return false; }
            (*strings)[key] = value;
        } else {
            char* end = 0;
            errno = 0;
            long value = strtol(input.c_str() + at, &end, 10);
            if (errno != 0 || end == input.c_str() + at) { *error = "configuration field must be a string or integer"; return false; }
            at = static_cast<std::string::size_type>(end - input.c_str());
            if (at < input.size() && !(input[at] == ',' || input[at] == '}' || input[at] == ' ' ||
                                       input[at] == '\t' || input[at] == '\r' || input[at] == '\n')) {
                *error = "configuration number is malformed"; return false;
            }
            if (strings->count(key) || numbers->count(key)) { *error = "duplicate configuration field: " + key; return false; }
            (*numbers)[key] = value;
        }
        if (at == start) { *error = "configuration field has no value"; return false; }
        skipWhitespace(input, &at);
        if (at < input.size() && input[at] == '}') {
            ++at; skipWhitespace(input, &at);
            if (at == input.size()) return true;
            *error = "unexpected data after configuration object"; return false;
        }
        if (at >= input.size() || input[at++] != ',') { *error = "configuration fields must be comma separated"; return false; }
        skipWhitespace(input, &at);
    }
    *error = "configuration object is not closed";
    return false;
}

bool stringSetting(const std::map<std::string, std::string>& strings,
                   const std::map<std::string, long>& numbers, const char* name,
                   std::string* value, std::string* error) {
    std::map<std::string, std::string>::const_iterator found = strings.find(name);
    if (found != strings.end()) { *value = found->second; return true; }
    if (numbers.find(name) != numbers.end()) { *error = std::string("configuration field must be a string: ") + name; return false; }
    return true;
}

bool numberSetting(const std::map<std::string, std::string>& strings,
                   const std::map<std::string, long>& numbers, const char* name,
                   long* value, std::string* error) {
    std::map<std::string, long>::const_iterator found = numbers.find(name);
    if (found != numbers.end()) { *value = found->second; return true; }
    if (strings.find(name) != strings.end()) { *error = std::string("configuration field must be an integer: ") + name; return false; }
    return true;
}
}

GatewayConfig::GatewayConfig() : zigguratSocket("/tmp/ziggurat/native.sock"), serialDevice("/dev/ttyS1"), serialBaud(115200), broadcastPassiveAckQuorum(8), matterCountryCode("DE"), matterRegulatoryLocation("indoor"), runtimeDirectory("/tuya/native"),
    httpPort(80), httpsPort(443), firebaseTopic("zigbee_alerts"), gatewayName("MicroHAD"), companionDiscovery(1) {}

bool selectGatewayConfigPath(int argc, const char* const argv[], std::string* path, std::string* error) {
    if (argc == 1) { *path = "/tuya/native/config.json"; return true; }
    if (argc == 3 && std::string(argv[1]) == "--config" && argv[2] != 0 && argv[2][0] != '\0') {
        *path = argv[2]; return true;
    }
    *error = "usage: microhad [--config /protected/path/config.json]";
    return false;
}

bool loadGatewayConfig(const std::string& path, GatewayConfig* config, std::string* error) {
    std::string input;
    std::map<std::string, std::string> strings;
    std::map<std::string, long> numbers;
    if (!readTextFile(path, &input)) { *error = "cannot read configuration " + path; return false; }
    if (!parseObject(input, &strings, &numbers, error)) return false;
    if (!stringSetting(strings, numbers, "ziggurat_socket", &config->zigguratSocket, error) ||
        !stringSetting(strings, numbers, "serial_device", &config->serialDevice, error) ||
        !numberSetting(strings, numbers, "serial_baud", &config->serialBaud, error) ||
        !numberSetting(strings, numbers, "broadcast_passive_ack_quorum", &config->broadcastPassiveAckQuorum, error) ||
        !stringSetting(strings, numbers, "matter_country_code", &config->matterCountryCode, error) ||
        !stringSetting(strings, numbers, "matter_regulatory_location", &config->matterRegulatoryLocation, error) ||
        !stringSetting(strings, numbers, "runtime_dir", &config->runtimeDirectory, error) ||
        !numberSetting(strings, numbers, "http_port", &config->httpPort, error) ||
        !numberSetting(strings, numbers, "https_port", &config->httpsPort, error) ||
        !stringSetting(strings, numbers, "firebase_service_account", &config->firebaseServiceAccount, error) ||
        !stringSetting(strings, numbers, "firebase_topic", &config->firebaseTopic, error) ||
        !stringSetting(strings, numbers, "gateway_name", &config->gatewayName, error) ||
        !numberSetting(strings, numbers, "companion_discovery", &config->companionDiscovery, error) ||
        !stringSetting(strings, numbers, "firebase_application_id", &config->firebaseApplicationId, error) ||
        !stringSetting(strings, numbers, "firebase_api_key", &config->firebaseApiKey, error) ||
        !stringSetting(strings, numbers, "firebase_project_id", &config->firebaseProjectId, error) ||
        !stringSetting(strings, numbers, "firebase_sender_id", &config->firebaseSenderId, error) ||
        !stringSetting(strings, numbers, "ca_bundle", &config->caBundle, error) ||
        !stringSetting(strings, numbers, "tls_certificate", &config->tlsCertificate, error) ||
        !stringSetting(strings, numbers, "tls_private_key", &config->tlsPrivateKey, error)) return false;
    if (config->gatewayName.empty() || config->gatewayName.size() > 128 ||
        config->gatewayName.find_first_of("\r\n\t") != std::string::npos) {
        *error = "gateway_name must contain 1-128 bytes without control whitespace"; return false;
    }
    if (config->companionDiscovery != 0 && config->companionDiscovery != 1) {
        *error = "companion_discovery must be 0 or 1"; return false;
    }
    bool hasFirebaseClient = !config->firebaseApplicationId.empty() || !config->firebaseApiKey.empty() ||
        !config->firebaseProjectId.empty() || !config->firebaseSenderId.empty();
    if (hasFirebaseClient) {
        const std::string values[] = {config->firebaseApplicationId, config->firebaseApiKey, config->firebaseProjectId, config->firebaseSenderId};
        for (unsigned int i=0; i<4; ++i) {
            if (values[i].empty() || values[i].size()>512) { *error="all four Firebase client fields are required (1-512 bytes each)"; return false; }
            for (size_t j=0; j<values[i].size(); ++j) if (static_cast<unsigned char>(values[i][j])<=32) {
                *error="Firebase client fields must not contain whitespace or control characters"; return false;
            }
        }
        if (config->firebaseSenderId.find_first_not_of("0123456789") != std::string::npos ||
            config->firebaseApplicationId.find("1:" + config->firebaseSenderId + ":android:") != 0 ||
            config->firebaseApplicationId.size() <= config->firebaseSenderId.size()+11) {
            *error="Firebase application ID must be an Android app belonging to firebase_sender_id"; return false;
        }
        std::string accountText, project;
        if (config->firebaseServiceAccount.empty() || !readTextFile(config->firebaseServiceAccount, &accountText) ||
            !jsonStringField(accountText, "project_id", &project) || project != config->firebaseProjectId) {
            *error="Firebase client project must match the configured sending service account"; return false;
        }
    }
    if (config->broadcastPassiveAckQuorum < 1 || config->broadcastPassiveAckQuorum > 255) {
        *error = "broadcast_passive_ack_quorum must be an integer between 1 and 255"; return false;
    }
    if (config->matterCountryCode.size() != 2 || config->matterCountryCode[0] < 'A' ||
        config->matterCountryCode[0] > 'Z' || config->matterCountryCode[1] < 'A' ||
        config->matterCountryCode[1] > 'Z') {
        *error = "matter_country_code must be a two-letter uppercase country code"; return false;
    }
    if (config->matterRegulatoryLocation != "indoor" && config->matterRegulatoryLocation != "outdoor") {
        *error = "matter_regulatory_location must be indoor or outdoor"; return false;
    }
    if (config->zigguratSocket.empty() || config->zigguratSocket[0] != '/' || config->zigguratSocket.size() >= 108) {
        *error = "ziggurat_socket must be an absolute Unix socket path shorter than 108 bytes"; return false;
    }
    if (config->serialBaud < 0 || config->serialBaud > INT_MAX) {
        *error = "serial_baud is outside the supported integer range"; return false;
    }
    if (config->serialBaud != 115200) {
        *error = "only serial_baud 115200 is supported"; return false;
    }
    if (config->httpPort < 1 || config->httpPort > 65535 || config->httpsPort < 1 || config->httpsPort > 65535) {
        *error = "listener ports must be between 1 and 65535"; return false;
    }
    if (config->tlsCertificate.empty() != config->tlsPrivateKey.empty()) {
        *error = "both tls_certificate and tls_private_key are required for HTTPS"; return false;
    }
    if (!config->tlsCertificate.empty() && !config->tlsPrivateKey.empty() && config->httpPort == config->httpsPort) {
        *error = "HTTP and HTTPS listener ports must be different"; return false;
    }
    if (config->runtimeDirectory.empty()) {
        *error = "runtime_dir must not be empty"; return false;
    }
    return true;
}

std::string GatewayConfig::companionJson() const {
    std::string body = "{\"schema\":1,\"name\":\"" + jsonEscape(gatewayName) + "\",\"matter\":";
#if MICROHAD_MATTER_ENABLED
    body += "true";
#else
    body += "false";
#endif
    body += ",\"firebase\":";
    if (firebaseApplicationId.empty()) body += "null";
    else body += "{\"application_id\":\"" + jsonEscape(firebaseApplicationId) + "\",\"api_key\":\"" + jsonEscape(firebaseApiKey) +
        "\",\"project_id\":\"" + jsonEscape(firebaseProjectId) + "\",\"sender_id\":\"" + jsonEscape(firebaseSenderId) + "\"}";
    return body + "}";
}
