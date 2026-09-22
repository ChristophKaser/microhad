#include "web_push.h"
#include "fcm.h"
#include "https_client.h"
#include "json_util.h"

#include <fstream>
#include <sstream>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <vector>

namespace {
const unsigned long kMaximumSubscriptions = 32;

bool validEndpoint(const std::string& endpoint) {
    const std::string prefix = "https://";
    std::string::size_type authorityEnd;
    unsigned long i;
    if (endpoint.compare(0, prefix.size(), prefix) != 0 || endpoint.size() <= prefix.size()) return false;
    authorityEnd = endpoint.find('/', prefix.size());
    if (authorityEnd == prefix.size()) return false;
    if (authorityEnd == std::string::npos) authorityEnd = endpoint.size();
    if (endpoint.find('@', prefix.size()) < authorityEnd) return false;
    for (i = 0; i < endpoint.size(); ++i)
        if (static_cast<unsigned char>(endpoint[i]) <= 0x20 || static_cast<unsigned char>(endpoint[i]) == 0x7f) return false;
    return true;
}

bool validSubscription(const std::string& subscriptionJson, std::string* endpoint) {
    std::string publicText, authText, clientPublic, auth;
    return jsonStringField(subscriptionJson, "endpoint", endpoint) && validEndpoint(*endpoint) &&
           jsonStringField(subscriptionJson, "p256dh", &publicText) &&
           jsonStringField(subscriptionJson, "auth", &authText) &&
           base64UrlDecode(publicText, &clientPublic) && clientPublic.size() == 65 &&
           base64UrlDecode(authText, &auth) && auth.size() == 16;
}

bool saveSubscriptions(const std::string& filename, const std::vector<std::string>& subscriptions,
                       std::string* error) {
    std::string temporary = filename + ".tmp";
    std::ofstream output(temporary.c_str());
    if (!output) { *error = "cannot persist Browser Push subscription"; return false; }
    for (unsigned long i = 0; i < subscriptions.size(); ++i) output << subscriptions[i] << "\n";
    output.close();
    if (!output || chmod(temporary.c_str(), 0600) != 0 || rename(temporary.c_str(), filename.c_str()) != 0) {
        remove(temporary.c_str());
        *error = "cannot persist Browser Push subscription";
        return false;
    }
    return true;
}

#ifdef MICROHAD_MIPS_TARGET
bool permanentlyRejected(const std::string& response) {
    std::string::size_type statusAt;
    char* end = 0;
    long status;
    if (response.compare(0, 5, "HTTP/") != 0) return false;
    statusAt = response.find(' ');
    if (statusAt == std::string::npos) return false;
    status = strtol(response.c_str() + statusAt + 1, &end, 10);
    return end != response.c_str() + statusAt + 1 && (status == 404 || status == 410);
}
#endif
}

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
std::string urlOrigin(const std::string& endpoint) {
    std::string::size_type scheme = endpoint.find("://");
    std::string::size_type path;
    if (scheme == std::string::npos) return "";
    path = endpoint.find('/', scheme + 3);
    return path == std::string::npos ? endpoint : endpoint.substr(0, path);
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
        chmod((runtimeDirectory_ + "/vapid-private.key").c_str(), 0600);
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
    publicOut.close();
    privateOut.close();
    if (!publicOut || !privateOut || chmod((runtimeDirectory_ + "/vapid-private.key").c_str(), 0600) != 0) {
        *error = "cannot persist VAPID key";
        return false;
    }
    return true;
#else
    *error = "VAPID key generation requires the MIPS crypto backend";
    return false;
#endif
}

bool WebPushStore::addSubscription(const std::string& subscriptionJson, std::string* error) {
    std::string endpoint;
    std::string filename = runtimeDirectory_ + "/web-push-subscriptions.ndjson";
    std::ifstream input(filename.c_str());
    std::vector<std::string> subscriptions;
    std::string existing, existingEndpoint;
    bool replaced = false;
    if (subscriptionJson.empty() || subscriptionJson.size() > 8192) {
        *error = "invalid Browser Push subscription";
        return false;
    }
    if (!validSubscription(subscriptionJson, &endpoint)) {
        *error = "invalid Browser Push subscription";
        return false;
    }
    while (std::getline(input, existing)) {
        if (!validSubscription(existing, &existingEndpoint)) continue;
        if (existingEndpoint == endpoint) {
            if (!replaced) subscriptions.push_back(subscriptionJson);
            replaced = true;
        } else {
            bool alreadyStored = false;
            for (unsigned long i = 0; i < subscriptions.size(); ++i) {
                std::string storedEndpoint;
                if (validSubscription(subscriptions[i], &storedEndpoint) && storedEndpoint == existingEndpoint) {
                    alreadyStored = true;
                    break;
                }
            }
            if (!alreadyStored) subscriptions.push_back(existing);
        }
    }
    if (!replaced) {
        if (subscriptions.size() >= kMaximumSubscriptions) {
            *error = "Browser Push subscription limit reached";
            return false;
        }
        subscriptions.push_back(subscriptionJson);
    }
    return saveSubscriptions(filename, subscriptions, error);
}

std::string WebPushStore::publicKey() const {
    return publicKey_;
}

bool WebPushStore::send(const std::string& caBundle, const std::string& title, const std::string& body,
                        std::string* error) {
#ifdef MICROHAD_MIPS_TARGET
    const std::string subscriptionFilename = runtimeDirectory_ + "/web-push-subscriptions.ndjson";
    std::ifstream subscriptions(subscriptionFilename.c_str());
    std::ifstream privateFile((runtimeDirectory_ + "/vapid-private.key").c_str(), std::ios::binary);
    std::string subscription;
    std::vector<std::string> retainedSubscriptions;
    std::string privateKey(32, '\0');
    bool delivered = false;
    bool pruneRequired = false;
    if (!subscriptions) { *error = "no Browser Push subscriptions"; return false; }
    if (!privateFile.read(&privateKey[0], privateKey.size())) { *error = "cannot read VAPID private key"; return false; }
    while (std::getline(subscriptions, subscription)) {
        std::string endpoint, publicText, authText, clientPublic, auth, encrypted(8192, '\0');
        std::string audience, header, jwtHeader, jwtPayload, jwtInput, signature(64, '\0'), token;
        unsigned long encryptedLength = 0;
        if (!validSubscription(subscription, &endpoint) || !jsonStringField(subscription, "p256dh", &publicText) ||
            !jsonStringField(subscription, "auth", &authText) || !base64UrlDecode(publicText, &clientPublic) ||
            !base64UrlDecode(authText, &auth) || clientPublic.size() != 65 || auth.size() != 16) {
            pruneRequired = true;
            continue;
        }
        retainedSubscriptions.push_back(subscription);
        std::string payload = "{\"title\":\"" + jsonEscape(title) + "\",\"body\":\"" + jsonEscape(body) + "\"}";
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
        std::string response, sendError;
        if (httpsPostUrl(endpoint, header, encrypted, caBundle, &response, &sendError)) {
            if (permanentlyRejected(response)) {
                retainedSubscriptions.pop_back();
                pruneRequired = true;
                continue;
            }
            delivered = true;
        }
    }
    subscriptions.close();
    if (pruneRequired) {
        std::string saveError;
        if (!saveSubscriptions(subscriptionFilename, retainedSubscriptions, &saveError)) {
            *error = saveError;
            return false;
        }
    }
    if (!delivered) { *error = "Browser Push delivery failed"; return false; }
    return true;
#else
    (void)caBundle; (void)title; (void)body;
    *error = "Browser Push delivery requires the MIPS crypto backend";
    return false;
#endif
}
