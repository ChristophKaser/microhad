#include "web_push.h"
#include "fcm.h"
#include "https_client.h"

#include <fstream>
#include <sstream>
#include <ctime>

#ifdef MICROHAD_MIPS_TARGET
extern "C" int microhad_vapid_generate(unsigned char* publicKey, unsigned char* privateKey);
extern "C" int microhad_web_push_encrypt(const unsigned char* clientPublic, const unsigned char* authSecret,
                                      const unsigned char* plaintext, unsigned long plaintextLength,
                                      unsigned char* output, unsigned long outputCapacity, unsigned long* outputLength);
extern "C" int microhad_vapid_sign(const unsigned char* privateKey, const unsigned char* input,
                                unsigned long inputLength, unsigned char* signature);
#endif

#ifdef MICROHAD_MIPS_TARGET
namespace {
bool jsonValue(const std::string& value, const std::string& name, std::string* result) {
    const std::string marker = "\"" + name + "\"";
    std::string::size_type start = value.find(marker);
    std::string::size_type quote;
    if (start == std::string::npos) return false;
    start = value.find(':', start + marker.size());
    if (start == std::string::npos) return false;
    quote = value.find('"', start + 1);
    if (quote == std::string::npos) return false;
    start = quote + 1;
    quote = value.find('"', start);
    if (quote == std::string::npos) return false;
    *result = value.substr(start, quote - start);
    return true;
}

std::string urlOrigin(const std::string& endpoint) {
    std::string::size_type scheme = endpoint.find("://");
    std::string::size_type path;
    if (scheme == std::string::npos) return "";
    path = endpoint.find('/', scheme + 3);
    return path == std::string::npos ? endpoint : endpoint.substr(0, path);
}

std::string jsonEscapePush(const std::string& value) {
    std::string output;
    std::string::size_type i;
    for (i = 0; i < value.size(); ++i) {
        if (value[i] == '"' || value[i] == '\\') output += '\\';
        output += value[i];
    }
    return output;
}
}
#endif

WebPushStore::WebPushStore(const std::string& runtimeDirectory) : runtimeDirectory_(runtimeDirectory) {
    std::string ignored;
    loadOrCreateKey(&ignored);
}

bool WebPushStore::loadOrCreateKey(std::string* error) {
    std::ifstream input((runtimeDirectory_ + "/vapid-public.key").c_str());
    if (input) {
        input >> publicKey_;
        return !publicKey_.empty();
    }
#ifdef MICROHAD_MIPS_TARGET
    unsigned char publicKey[65];
    unsigned char privateKey[32];
    if (microhad_vapid_generate(publicKey, privateKey) != 0) { *error = "cannot generate VAPID key"; return false; }
    publicKey_ = base64UrlEncode(std::string(reinterpret_cast<char*>(publicKey), sizeof(publicKey)));
    std::ofstream publicOut((runtimeDirectory_ + "/vapid-public.key").c_str());
    std::ofstream privateOut((runtimeDirectory_ + "/vapid-private.key").c_str(), std::ios::binary);
    if (!publicOut || !privateOut) { *error = "cannot persist VAPID key"; return false; }
    publicOut << publicKey_ << "\n";
    privateOut.write(reinterpret_cast<char*>(privateKey), sizeof(privateKey));
    return static_cast<bool>(publicOut) && static_cast<bool>(privateOut);
#else
    *error = "VAPID key generation requires the MIPS crypto backend";
    return false;
#endif
}

bool WebPushStore::addSubscription(const std::string& subscriptionJson, std::string* error) {
    if (subscriptionJson.empty() || subscriptionJson.size() > 8192) {
        *error = "invalid Browser Push subscription";
        return false;
    }
    std::ofstream output((runtimeDirectory_ + "/web-push-subscriptions.ndjson").c_str(), std::ios::app);
    if (!output) { *error = "cannot persist Browser Push subscription"; return false; }
    output << subscriptionJson << "\n";
    return true;
}

std::string WebPushStore::publicKey() const {
    return publicKey_;
}

bool WebPushStore::send(const std::string& caBundle, const std::string& title, const std::string& body,
                        std::string* error) {
#ifdef MICROHAD_MIPS_TARGET
    std::ifstream subscriptions((runtimeDirectory_ + "/web-push-subscriptions.ndjson").c_str());
    std::ifstream privateFile((runtimeDirectory_ + "/vapid-private.key").c_str(), std::ios::binary);
    std::string subscription;
    std::string privateKey(32, '\0');
    bool delivered = false;
    if (!subscriptions) { *error = "no Browser Push subscriptions"; return false; }
    if (!privateFile.read(&privateKey[0], privateKey.size())) { *error = "cannot read VAPID private key"; return false; }
    while (std::getline(subscriptions, subscription)) {
        std::string endpoint, publicText, authText, clientPublic, auth, encrypted(8192, '\0');
        std::string audience, header, jwtHeader, jwtPayload, jwtInput, signature(64, '\0'), token;
        unsigned long encryptedLength = 0;
        if (!jsonValue(subscription, "endpoint", &endpoint) || !jsonValue(subscription, "p256dh", &publicText) ||
            !jsonValue(subscription, "auth", &authText) || !base64UrlDecode(publicText, &clientPublic) ||
            !base64UrlDecode(authText, &auth) || clientPublic.size() != 65 || auth.size() != 16) continue;
        std::string payload = "{\"title\":\"" + jsonEscapePush(title) + "\",\"body\":\"" + jsonEscapePush(body) + "\"}";
        payload += static_cast<char>(2);
        if (microhad_web_push_encrypt(reinterpret_cast<const unsigned char*>(clientPublic.data()),
                                  reinterpret_cast<const unsigned char*>(auth.data()),
                                  reinterpret_cast<const unsigned char*>(payload.data()), payload.size(),
                                  reinterpret_cast<unsigned char*>(&encrypted[0]), encrypted.size(), &encryptedLength) != 0) continue;
        encrypted.resize(encryptedLength);
        audience = urlOrigin(endpoint);
        jwtHeader = base64UrlEncode("{\"typ\":\"JWT\",\"alg\":\"ES256\"}");
        std::ostringstream claims;
        claims << "{\"aud\":\"" << audience << "\",\"exp\":" << (time(0) + 43200) << ",\"sub\":\"mailto:admin@localhost\"}";
        jwtPayload = base64UrlEncode(claims.str());
        jwtInput = jwtHeader + "." + jwtPayload;
        if (microhad_vapid_sign(reinterpret_cast<const unsigned char*>(privateKey.data()),
                            reinterpret_cast<const unsigned char*>(jwtInput.data()), jwtInput.size(),
                            reinterpret_cast<unsigned char*>(&signature[0])) != 0) continue;
        token = jwtInput + "." + base64UrlEncode(signature);
        header = "Content-Type: application/octet-stream\r\nContent-Encoding: aes128gcm\r\nTTL: 300\r\nAuthorization: vapid t=" + token + ", k=" + publicKey_ + "\r\n";
        std::string ignored;
        if (httpsPostUrl(endpoint, header, encrypted, caBundle, &ignored, &ignored)) delivered = true;
    }
    if (!delivered) { *error = "Browser Push delivery failed"; return false; }
    return true;
#else
    (void)caBundle; (void)title; (void)body;
    *error = "Browser Push delivery requires the MIPS crypto backend";
    return false;
#endif
}
