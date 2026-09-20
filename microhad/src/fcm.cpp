#include "fcm.h"
#include "storage.h"

#include <cstdio>

#ifdef MICROHAD_MIPS_TARGET
extern "C" int microhad_fcm_sign(const char* pem, const unsigned char* input, size_t inputLength,
                              unsigned char* signature, size_t* signatureLength);
#endif

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

std::string makeGoogleAccessTokenJwt(const FirebaseServiceAccount& account, long issuedAt, std::string* error) {
#ifdef MICROHAD_MIPS_TARGET
    std::string header = base64UrlEncode("{\"alg\":\"RS256\",\"typ\":\"JWT\"}");
    std::string claims = std::string("{\"iss\":\"") + jsonEscape(account.clientEmail) +
        "\",\"scope\":\"https://www.googleapis.com/auth/firebase.messaging\",\"aud\":\"https://oauth2.googleapis.com/token\",\"iat\":";
    char numeric[32];
    snprintf(numeric, sizeof(numeric), "%ld", issuedAt);
    claims += numeric;
    claims += ",\"exp\":";
    snprintf(numeric, sizeof(numeric), "%ld", issuedAt + 3600);
    claims += numeric;
    claims += "}";
    std::string signingInput = header + "." + base64UrlEncode(claims);
    unsigned char signature[512]; /* supports service-account RSA keys up to 4096 bits */
    size_t signatureLength = 0;
    if (microhad_fcm_sign(account.privateKeyPem.c_str(), reinterpret_cast<const unsigned char*>(signingInput.data()),
                      signingInput.size(), signature, &signatureLength) != 0) {
        *error = "cannot sign Firebase OAuth JWT";
        return std::string();
    }
    return signingInput + "." + base64UrlEncode(std::string(reinterpret_cast<char*>(signature), signatureLength));
#else
    (void)account; (void)issuedAt;
    *error = "JWT signing is available only in the RSDK MIPS build";
    return std::string();
#endif
}
