#include "fcm.h"
#include "json_util.h"
#include "storage.h"
#include "web_push.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace {
std::string subscription(const std::string& endpoint, unsigned char publicPrefix, unsigned long publicLength,
                         unsigned long authLength) {
    std::string publicKey(publicLength, '\0');
    std::string auth(authLength, '\0');
    if (!publicKey.empty()) publicKey[0] = static_cast<char>(publicPrefix);
    return "{\"endpoint\":\"" + endpoint + "\",\"keys\":{\"p256dh\":\"" +
           base64UrlEncode(publicKey) + "\",\"auth\":\"" + base64UrlEncode(auth) + "\"}}";
}

std::string subscription(const std::string& endpoint) {
    return subscription(endpoint, 4, 65, 16);
}

void testVapidKeys(const std::string& directory) {
    std::string error;
    std::string publicKey;
    std::string privateKey;
    std::string decoded;
    std::string keyDirectory = directory + "/keys";
    assert(ensureDirectory(directory, &error));
    assert(ensureDirectory(keyDirectory, &error));
    {
        WebPushStore first(keyDirectory);
        publicKey = first.publicKey();
        assert(base64UrlDecode(publicKey, &decoded) && decoded.size() == 65 && static_cast<unsigned char>(decoded[0]) == 4);
    }
    assert(readTextFile(keyDirectory + "/vapid-private.key", &privateKey) && privateKey.size() == 32);
    {
        WebPushStore reloaded(keyDirectory);
        assert(reloaded.publicKey() == publicKey);
    }
    {
        std::ofstream corrupt((keyDirectory + "/vapid-public.key").c_str());
        corrupt << "corrupt";
    }
    {
        WebPushStore corrupt(keyDirectory);
        assert(corrupt.publicKey().empty());
    }
    assert(readTextFile(keyDirectory + "/vapid-public.key", &decoded) && decoded == "corrupt");
    assert(readTextFile(keyDirectory + "/vapid-private.key", &decoded) && decoded == privateKey);
    unlink((keyDirectory + "/vapid-public.key").c_str());
    {
        WebPushStore partial(keyDirectory);
        assert(partial.publicKey().empty());
    }
    {
        std::ofstream blocked((directory + "/blocked").c_str());
        assert(blocked);
        blocked << "not a directory";
    }
    {
        WebPushStore unwritable(directory + "/blocked");
        assert(unwritable.publicKey().empty());
    }
    unlink((keyDirectory + "/vapid-private.key").c_str());
    rmdir(keyDirectory.c_str());
    unlink((directory + "/blocked").c_str());
    rmdir(directory.c_str());
}
}

void runWebPushUnitTests() {
    std::ostringstream path;
    std::string error;
    path << "/tmp/microhad-web-push-unit-" << static_cast<long>(getpid());
    fprintf(stderr, "unit: Web Push subscription validation and persistence\n");
    assert(ensureDirectory(path.str(), &error));
    testVapidKeys(path.str() + "-keys");
    WebPushStore store(path.str());
    const std::string endpoint = "https://push.example.test/subscription";
    assert(store.addSubscription(subscription(endpoint), &error));
    assert(store.addSubscription(subscription(endpoint), &error)); /* replace, never duplicate */
    assert(!store.addSubscription("{\"endpoint\":\"http://invalid\"}", &error));
    assert(!store.addSubscription("not JSON", &error));
    assert(!store.addSubscription("{\"endpoint\":\"https://push.example.test/a\",\"keys\":{\"p256dh\":\"!\",\"auth\":\"!\"}}", &error));
    assert(!store.addSubscription(subscription("https://push.example.test/bad-prefix", 2, 65, 16), &error));
    assert(!store.addSubscription(subscription("https://push.example.test/bad-public-length", 4, 64, 16), &error));
    assert(!store.addSubscription(subscription("https://push.example.test/bad-auth-length", 4, 65, 15), &error));
    for (int i = 0; i < 31; ++i) {
        std::ostringstream endpointNumber;
        endpointNumber << "https://push.example.test/" << i;
        assert(store.addSubscription(subscription(endpointNumber.str()), &error));
    }
    assert(!store.addSubscription(subscription("https://push.example.test/overflow"), &error));
    assert(error == "Browser Push subscription limit reached");
    unlink((path.str() + "/web-push-subscriptions.ndjson").c_str());
    unlink((path.str() + "/web-push-clients.ndjson").c_str());
    unlink((path.str() + "/vapid-public.key").c_str());
    unlink((path.str() + "/vapid-private.key").c_str());
    rmdir(path.str().c_str());
}
