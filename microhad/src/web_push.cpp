#include "web_push.h"
#include "fcm.h"

#include <fstream>

#ifdef MICROHAD_MIPS_TARGET
extern "C" int microhad_vapid_generate(unsigned char* publicKey, unsigned char* privateKey);
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
