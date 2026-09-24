#include "fcm.h"
#include "storage.h"
#include "web_push.h"

#include <cassert>
#include <cstdio>
#include <sstream>
#include <unistd.h>

namespace {
std::string subscription() {
    std::string publicKey(65, '\0');
    std::string auth(16, '\0');
    publicKey[0] = 4;
    return "{\"endpoint\":\"https://push.example.test/subscription\",\"keys\":{\"p256dh\":\"" +
           base64UrlEncode(publicKey) + "\",\"auth\":\"" + base64UrlEncode(auth) + "\"}}";
}
}

void runWebPushUnitTests() {
    std::ostringstream path;
    std::string error;
    path << "/tmp/microhad-web-push-unit-" << static_cast<long>(getpid());
    fprintf(stderr, "unit: Web Push subscription validation and persistence\n");
    assert(ensureDirectory(path.str(), &error));
    WebPushStore store(path.str());
    assert(store.addSubscription(subscription(), &error));
    assert(!store.addSubscription("{\"endpoint\":\"http://invalid\"}", &error));
    unlink((path.str() + "/web-push-subscriptions.ndjson").c_str());
    unlink((path.str() + "/vapid-public.key").c_str());
    unlink((path.str() + "/vapid-private.key").c_str());
    rmdir(path.str().c_str());
}
