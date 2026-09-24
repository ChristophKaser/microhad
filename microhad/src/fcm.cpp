#include "fcm.h"
#include "json_util.h"
#include "https_client.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>

#ifdef MICROHAD_MBEDTLS_ENABLED
extern "C" int microhad_fcm_sign(const char* pem, const unsigned char* input, size_t inputLength,
                              unsigned char* signature, size_t* signatureLength);
#endif

namespace {
std::string cachedToken;
std::string cachedTokenSource;
long cachedTokenExpiresAt = 0;

}

bool parseFirebaseServiceAccount(const std::string& json, FirebaseServiceAccount* account, std::string* error) {
    if (!jsonStringField(json, "project_id", &account->projectId) ||
        !jsonStringField(json, "client_email", &account->clientEmail) ||
        !jsonStringField(json, "private_key", &account->privateKeyPem) ||
        account->projectId.empty() || account->clientEmail.empty() || account->privateKeyPem.empty()) {
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

bool base64UrlDecode(const std::string& value, std::string* decoded) {
    unsigned int bits = 0;
    int available = 0;
    unsigned long i;
    decoded->clear();
    for (i = 0; i < value.size(); ++i) {
        unsigned char c = value[i];
        int digit = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 :
                    c >= '0' && c <= '9' ? c - '0' + 52 : c == '-' ? 62 : c == '_' ? 63 : -1;
        if (digit < 0) return false;
        bits = (bits << 6) | static_cast<unsigned int>(digit);
        available += 6;
        if (available >= 8) {
            available -= 8;
            decoded->push_back(static_cast<char>((bits >> available) & 0xff));
        }
    }
    return available == 0 || available == 2 || available == 4;
}

std::string makeFcmMessage(const std::string& topic, const std::string& title, const std::string& body) {
    return std::string("{\"message\":{\"topic\":\"") + jsonEscape(topic) + "\",\"notification\":{\"title\":\"" +
           jsonEscape(title) + "\",\"body\":\"" + jsonEscape(body) + "\"},\"android\":{\"priority\":\"high\"}}}";
}

std::string makeGoogleAccessTokenJwt(const FirebaseServiceAccount& account, long issuedAt, std::string* error) {
#ifdef MICROHAD_MBEDTLS_ENABLED
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

bool sendFcmNotification(const std::string& serviceAccountPath, const std::string& caBundle,
                         const std::string& topic, const std::string& title, const std::string& body,
                         std::string* error) {
    std::ifstream file(serviceAccountPath.c_str());
    FirebaseServiceAccount account;
    std::string contents;
    std::string tokenResponse;
    std::string response;
    std::string jwt;
    std::string token;
    std::string::size_type tokenAt;
    long now = time(0);
    if (!file) { *error = "cannot read Firebase service account"; return false; }
    contents.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!parseFirebaseServiceAccount(contents, &account, error)) return false;
    if (cachedTokenSource == serviceAccountPath + "\n" + caBundle && cachedTokenExpiresAt > now + 60) {
        token = cachedToken;
    } else {
        jwt = makeGoogleAccessTokenJwt(account, now, error);
        if (jwt.empty()) return false;
        if (!httpsPost("oauth2.googleapis.com", "/token", "Content-Type: application/x-www-form-urlencoded\r\n",
                       "grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=" + jwt, caBundle,
                       &tokenResponse, error)) return false;
        tokenAt = tokenResponse.find("\"access_token\":\"");
        if (tokenAt == std::string::npos) { *error = "OAuth response did not contain an access token"; return false; }
        tokenAt += 16;
        std::string::size_type tokenEnd = tokenResponse.find('"', tokenAt);
        if (tokenEnd == std::string::npos) { *error = "OAuth response has invalid access token"; return false; }
        token = tokenResponse.substr(tokenAt, tokenEnd - tokenAt);
        long expiresIn = 0;
        jsonLongField(tokenResponse, "expires_in", &expiresIn);
        if (expiresIn <= 60) { *error = "OAuth response has invalid token expiry"; return false; }
        cachedToken = token;
        cachedTokenSource = serviceAccountPath + "\n" + caBundle;
        cachedTokenExpiresAt = now + expiresIn;
    }
    if (!httpsPost("fcm.googleapis.com", "/v1/projects/" + account.projectId + "/messages:send",
                   "Content-Type: application/json\r\nAuthorization: Bearer " + token + "\r\n",
                   makeFcmMessage(topic, title, body), caBundle, &response, error)) return false;
    if (response.find("HTTP/1.1 200") != 0) { *error = "FCM rejected the notification"; return false; }
    return true;
}
