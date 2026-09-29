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
#include <stdint.h>
#include <iomanip>

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
           static_cast<unsigned char>(clientPublic[0]) == 4 &&
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

struct BrowserClient { std::string endpoint, label; bool enabled; };
std::vector<BrowserClient> loadClients(const std::string& directory) {
    std::vector<BrowserClient> clients;
    std::ifstream subscriptions((directory + "/web-push-subscriptions.ndjson").c_str());
    std::ifstream settings((directory + "/web-push-clients.ndjson").c_str());
    std::string line;
    while (std::getline(settings, line)) {
        BrowserClient client; long enabled = 1;
        if (jsonStringField(line, "endpoint", &client.endpoint) && jsonStringField(line, "label", &client.label) &&
            jsonLongField(line, "enabled", &enabled) && (enabled == 0 || enabled == 1)) {
            client.enabled = enabled != 0; clients.push_back(client);
        }
    }
    while (std::getline(subscriptions, line)) {
        std::string endpoint;
        if (!validSubscription(line, &endpoint)) continue;
        bool found = false;
        for (unsigned long i = 0; i < clients.size(); ++i) if (clients[i].endpoint == endpoint) found = true;
        if (!found) { BrowserClient client; client.endpoint = endpoint; client.label = "Browser"; client.enabled = true; clients.push_back(client); }
    }
    return clients;
}
std::string clientId(const std::string& endpoint) {
    uint32_t first = 2166136261U, second = 2246822519U;
    for (unsigned long i = 0; i < endpoint.size(); ++i) {
        uint32_t byte = static_cast<unsigned char>(endpoint[i]);
        first = (first ^ byte) * 16777619U;
        second = (second ^ (byte + static_cast<uint32_t>(i))) * 3266489917U;
    }
    std::ostringstream out;
    out << "browser-" << std::hex << std::setfill('0') << std::setw(8) << first << std::setw(8) << second;
    return out.str();
}
bool saveClients(const std::string& directory, const std::vector<BrowserClient>& clients, std::string* error) {
    const std::string filename = directory + "/web-push-clients.ndjson", temporary = filename + ".tmp";
    std::ofstream output(temporary.c_str());
    if (!output) { *error = "cannot persist Browser Push settings"; return false; }
    for (unsigned long i = 0; i < clients.size(); ++i)
        output << "{\"endpoint\":\"" << jsonEscape(clients[i].endpoint) << "\",\"label\":\"" << jsonEscape(clients[i].label)
               << "\",\"enabled\":" << (clients[i].enabled ? 1 : 0) << "}\n";
    output.close();
    if (!output || chmod(temporary.c_str(), 0600) != 0 || rename(temporary.c_str(), filename.c_str()) != 0) {
        remove(temporary.c_str()); *error = "cannot persist Browser Push settings"; return false;
    }
    return true;
}

#ifdef MICROHAD_MBEDTLS_ENABLED
long responseStatus(const std::string& response) {
    std::string::size_type statusAt;
    char* end = 0;
    if (response.compare(0, 5, "HTTP/") != 0) return -1;
    statusAt = response.find(' ');
    if (statusAt == std::string::npos) return -1;
    long status = strtol(response.c_str() + statusAt + 1, &end, 10);
    return end != response.c_str() + statusAt + 1 ? status : -1;
}

bool permanentlyRejected(const std::string& response) {
    long status = responseStatus(response);
    return status == 404 || status == 410;
}
#endif
}

#ifdef MICROHAD_MBEDTLS_ENABLED
extern "C" int microhad_vapid_generate(unsigned char* publicKey, unsigned char* privateKey);
extern "C" int microhad_web_push_encrypt(const unsigned char* clientPublic, const unsigned char* authSecret,
                                      const unsigned char* plaintext, unsigned long plaintextLength,
                                      unsigned char* output, unsigned long outputCapacity, unsigned long* outputLength);
extern "C" int microhad_vapid_sign(const unsigned char* privateKey, const unsigned char* input,
                                unsigned long inputLength, unsigned char* signature);
#endif

#ifdef MICROHAD_MBEDTLS_ENABLED
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
    std::ifstream privateInput((runtimeDirectory_ + "/vapid-private.key").c_str(), std::ios::binary);
    if (input) {
        std::string decoded;
        std::string privateKey(32, '\0');
        input >> publicKey_;
        if (!base64UrlDecode(publicKey_, &decoded) || decoded.size() != 65 ||
            static_cast<unsigned char>(decoded[0]) != 4 ||
            !privateInput.read(&privateKey[0], privateKey.size()) || privateInput.get() != EOF) {
            publicKey_.clear();
            *error = "invalid persisted VAPID key";
            return false;
        }
        if (chmod((runtimeDirectory_ + "/vapid-private.key").c_str(), 0600) != 0) {
            publicKey_.clear();
            *error = "cannot protect VAPID private key";
            return false;
        }
        return true;
    }
    if (privateInput) { *error = "incomplete persisted VAPID key"; return false; }
#ifdef MICROHAD_MBEDTLS_ENABLED
    unsigned char publicKey[65];
    unsigned char privateKey[32];
    if (microhad_vapid_generate(publicKey, privateKey) != 0) { *error = "cannot generate VAPID key"; return false; }
    publicKey_ = base64UrlEncode(std::string(reinterpret_cast<char*>(publicKey), sizeof(publicKey)));
    std::ofstream publicOut((runtimeDirectory_ + "/vapid-public.key").c_str());
    std::ofstream privateOut((runtimeDirectory_ + "/vapid-private.key").c_str(), std::ios::binary);
    if (!publicOut || !privateOut) { publicKey_.clear(); *error = "cannot persist VAPID key"; return false; }
    publicOut << publicKey_ << "\n";
    privateOut.write(reinterpret_cast<char*>(privateKey), sizeof(privateKey));
    publicOut.close();
    privateOut.close();
    if (!publicOut || !privateOut || chmod((runtimeDirectory_ + "/vapid-private.key").c_str(), 0600) != 0) {
        publicKey_.clear();
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
    if (!saveSubscriptions(filename, subscriptions, error)) return false;
    return saveClients(runtimeDirectory_, loadClients(runtimeDirectory_), error);
}

std::string WebPushStore::clientsJson() const {
    std::vector<BrowserClient> clients = loadClients(runtimeDirectory_);
    std::string json = "{\"clients\":[";
    for (unsigned long i = 0; i < clients.size(); ++i) {
        if (i) json += ",";
        json += "{\"id\":\"" + clientId(clients[i].endpoint) + "\",\"model\":\"Browser\",\"label\":\"" +
                jsonEscape(clients[i].label) + "\",\"enabled\":" + (clients[i].enabled ? "true}" : "false}");
    }
    return json + "]}";
}

bool WebPushStore::updateClient(const std::string& id, const std::string& label, bool enabled, std::string* error) {
    if (label.size() > 64) { *error = "invalid push client label"; return false; }
    std::vector<BrowserClient> clients = loadClients(runtimeDirectory_);
    for (unsigned long i = 0; i < clients.size(); ++i) if (clientId(clients[i].endpoint) == id) {
        clients[i].label = label; clients[i].enabled = enabled;
        return saveClients(runtimeDirectory_, clients, error);
    }
    *error = "Browser Push client not found"; return false;
}

bool WebPushStore::removeClient(const std::string& id, std::string* error) {
    const std::string filename = runtimeDirectory_ + "/web-push-subscriptions.ndjson";
    std::vector<BrowserClient> clients = loadClients(runtimeDirectory_);
    std::string endpoint;
    for (unsigned long i = 0; i < clients.size(); ++i) if (clientId(clients[i].endpoint) == id) endpoint = clients[i].endpoint;
    if (endpoint.empty()) { *error = "Browser Push client not found"; return false; }
    std::ifstream input(filename.c_str()); std::string line, storedEndpoint;
    std::vector<std::string> subscriptions;
    while (std::getline(input, line)) if (validSubscription(line, &storedEndpoint) && storedEndpoint != endpoint) subscriptions.push_back(line);
    if (!saveSubscriptions(filename, subscriptions, error)) return false;
    std::vector<BrowserClient> retained;
    for (unsigned long i = 0; i < clients.size(); ++i) if (clients[i].endpoint != endpoint) retained.push_back(clients[i]);
    return saveClients(runtimeDirectory_, retained, error);
}

std::string WebPushStore::publicKey() const {
    return publicKey_;
}

bool WebPushStore::send(const std::string& caBundle, const std::string& title, const std::string& body,
                        std::string* error) {
#ifdef MICROHAD_MBEDTLS_ENABLED
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
        std::vector<BrowserClient> clients = loadClients(runtimeDirectory_);
        bool enabled = true;
        for (unsigned long i = 0; i < clients.size(); ++i) if (clients[i].endpoint == endpoint) enabled = clients[i].enabled;
        if (!enabled) { retainedSubscriptions.push_back(subscription); continue; }
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
            long status = responseStatus(response);
            if (status >= 200 && status < 300) delivered = true;
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
