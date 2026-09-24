#include "fcm.h"
#include "https_client.h"
#include "json_util.h"
#include "notification_worker.h"
#include "web_push.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" int microhad_vapid_generate(unsigned char* publicKey, unsigned char* privateKey);

namespace {
struct HttpsRequest {
    std::string host;
    std::string path;
    std::string headers;
    std::string body;
};

class FakeHttpsTransport : public HttpsPostHook {
public:
    FakeHttpsTransport() : oauthFailure(false), malformedOAuth(false), failPush(false), expiresIn(3600), pushStatus(201) {}
    bool oauthFailure;
    bool malformedOAuth;
    bool failPush;
    long expiresIn;
    long pushStatus;
    std::string fcmResponse;
    std::string oauthError;
    std::vector<HttpsRequest> requests;

    bool post(const std::string& host, const std::string& path, const std::string& headers,
              const std::string& body, const std::string&, std::string* response, std::string* error) {
        HttpsRequest request;
        request.host = host; request.path = path; request.headers = headers; request.body = body;
        requests.push_back(request);
        if (host == "oauth2.googleapis.com") {
            if (oauthFailure) { *error = oauthError.empty() ? "fake OAuth connection failure" : oauthError; return false; }
            if (malformedOAuth) *response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
            else {
                std::ostringstream json;
                json << "HTTP/1.1 200 OK\r\nContent-Length: 64\r\n\r\n{\"access_token\":\"fake-token\",\"expires_in\":" << expiresIn << "}";
                *response = json.str();
            }
            return true;
        }
        if (host == "fcm.googleapis.com") { *response = fcmResponse; return true; }
        if (failPush) { *error = "fake Push connection failure"; return false; }
        std::ostringstream push;
        push << "HTTP/1.1 " << pushStatus << " Fake Push\r\nContent-Length: 0\r\n\r\n";
        *response = push.str();
        return true;
    }

    unsigned int countHost(const std::string& host) const {
        unsigned int count = 0;
        for (unsigned long i = 0; i < requests.size(); ++i) if (requests[i].host == host) ++count;
        return count;
    }
};

std::string makeAccount(const std::string& path, const std::string& email) {
    std::string key;
    std::ofstream output(path.c_str());
    assert(readTextFile("test/fixtures/tls/localhost.key", &key));
    assert(output);
    output << "{\"project_id\":\"project\",\"client_email\":\"" << jsonEscape(email)
           << "\",\"private_key\":\"" << jsonEscape(key) << "\"}";
    output.close();
    assert(output);
    return path;
}

void testFcmTokenCacheAndFailures(const std::string& directory, FakeHttpsTransport* transport) {
    std::string error;
    const std::string first = makeAccount(directory + "/account-one.json", "one@example.test");
    const std::string second = makeAccount(directory + "/account-two.json", "two@example.test");
    const std::string success = "HTTP/1.1 200 OK\r\nContent-Length: 36\r\n\r\n{\"name\":\"projects/p/messages/m\"}";
    transport->fcmResponse = success;
    bool sent = sendFcmNotification(first, "fake-ca", "alerts", "Window", "Kitchen", &error);
    if (!sent) fprintf(stderr, "fake FCM success response failed: %s\n", error.c_str());
    assert(sent);
    assert(sendFcmNotification(first, "fake-ca", "alerts", "Window", "Kitchen", &error));
    assert(transport->countHost("oauth2.googleapis.com") == 1);
    assert(transport->countHost("fcm.googleapis.com") == 2);
    bool foundFcmRequest = false;
    for (unsigned long i = 0; i < transport->requests.size(); ++i) {
        const HttpsRequest& request = transport->requests[i];
        if (request.host == "fcm.googleapis.com") {
            foundFcmRequest = true;
            assert(request.path == "/v1/projects/project/messages:send");
            assert(request.headers.find("Authorization: Bearer fake-token") != std::string::npos);
            assert(request.body.find("\"topic\":\"alerts\"") != std::string::npos);
        }
    }
    assert(foundFcmRequest);

    unsigned int oauthBefore = transport->countHost("oauth2.googleapis.com");
    transport->oauthFailure = true;
    assert(!sendFcmNotification(first, "fake-ca-failure", "alerts", "Window", "Kitchen", &error));
    assert(error == "fake OAuth connection failure");
    assert(transport->countHost("oauth2.googleapis.com") == oauthBefore + 1);
    transport->oauthFailure = false;

    transport->malformedOAuth = true;
    assert(!sendFcmNotification(first, "fake-ca-malformed", "alerts", "Window", "Kitchen", &error));
    assert(error == "OAuth response did not contain an access token");
    transport->malformedOAuth = false;

    transport->fcmResponse = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
    assert(!sendFcmNotification(second, "fake-ca-status", "alerts", "Window", "Kitchen", &error));
    assert(error == "FCM rejected the notification");
    transport->fcmResponse = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
    assert(!sendFcmNotification(second, "fake-ca-malformed-fcm", "alerts", "Window", "Kitchen", &error));
    assert(error == "FCM response was malformed");

    unsigned int beforeAccountChange = transport->countHost("oauth2.googleapis.com");
    transport->fcmResponse = success;
    assert(sendFcmNotification(second, "fake-ca-account-change", "alerts", "Window", "Kitchen", &error));
    assert(transport->countHost("oauth2.googleapis.com") == beforeAccountChange + 1);

    transport->expiresIn = 61;
    const std::string expiring = makeAccount(directory + "/account-expiring.json", "expiry@example.test");
    unsigned int beforeExpiry = transport->countHost("oauth2.googleapis.com");
    assert(sendFcmNotification(expiring, "fake-ca-expiry", "alerts", "Window", "Kitchen", &error));
    sleep(2);
    assert(sendFcmNotification(expiring, "fake-ca-expiry", "alerts", "Window", "Kitchen", &error));
    assert(transport->countHost("oauth2.googleapis.com") == beforeExpiry + 2);
}

std::string makeBrowserSubscription(const std::string& endpoint) {
    unsigned char publicKey[65];
    unsigned char privateKey[32];
    std::string auth(16, '\0');
    assert(microhad_vapid_generate(publicKey, privateKey) == 0);
    return std::string("{\"endpoint\":\"") + endpoint + "\",\"keys\":{\"p256dh\":\"" +
        base64UrlEncode(std::string(reinterpret_cast<char*>(publicKey), sizeof(publicKey))) +
        "\",\"auth\":\"" + base64UrlEncode(auth) + "\"}}";
}

void prepareBrowserSubscription(const std::string& directory) {
    WebPushStore store(directory);
    std::string error;
    assert(!store.publicKey().empty());
    assert(store.addSubscription(makeBrowserSubscription("https://push.example.test/subscription"), &error));
}

void cleanNotificationRuntime(const std::string& directory) {
    unlink((directory + "/notifications.json").c_str());
    unlink((directory + "/vapid-public.key").c_str());
    unlink((directory + "/vapid-private.key").c_str());
    unlink((directory + "/web-push-subscriptions.ndjson").c_str());
    rmdir(directory.c_str());
}

void testNotificationTransportIndependence(const std::string& root) {
    std::string error;
    std::string directory = root + "/browser-only";
    assert(mkdir(directory.c_str(), 0700) == 0);
    prepareBrowserSubscription(directory);
    FakeHttpsTransport browserOnly;
    setHttpsPostHook(&browserOnly);
    {
        NotificationWorker worker(directory, "", "", "fake-ca");
        assert(worker.start(&error));
        assert(worker.enqueue("Test", "browser only", &error));
        worker.stop();
    }
    assert(browserOnly.countHost("push.example.test") == 1);
    cleanNotificationRuntime(directory);

    directory = root + "/fcm-failure";
    assert(mkdir(directory.c_str(), 0700) == 0);
    prepareBrowserSubscription(directory);
    const std::string account = makeAccount(directory + "/service-account.json", "worker@example.test");
    FakeHttpsTransport fcmFailure;
    fcmFailure.oauthFailure = true;
    setHttpsPostHook(&fcmFailure);
    {
        NotificationWorker worker(directory, account, "alerts", "fake-ca-worker-failure");
        assert(worker.start(&error));
        assert(worker.enqueue("Test", "fallback delivery", &error));
        worker.stop();
    }
    assert(fcmFailure.countHost("oauth2.googleapis.com") == 1);
    assert(fcmFailure.countHost("fcm.googleapis.com") == 0);
    assert(fcmFailure.countHost("push.example.test") == 1);
    unlink(account.c_str());
    cleanNotificationRuntime(directory);

    directory = root + "/push-failure";
    assert(mkdir(directory.c_str(), 0700) == 0);
    prepareBrowserSubscription(directory);
    const std::string secondAccount = makeAccount(directory + "/service-account.json", "worker-two@example.test");
    FakeHttpsTransport pushFailure;
    pushFailure.failPush = true;
    pushFailure.fcmResponse = "HTTP/1.1 200 OK\r\nContent-Length: 34\r\n\r\n{\"name\":\"projects/p/messages/m\"}";
    setHttpsPostHook(&pushFailure);
    {
        NotificationWorker worker(directory, secondAccount, "alerts", "fake-ca-push-failure");
        assert(worker.start(&error));
        assert(worker.enqueue("Test", "FCM first", &error));
        worker.stop();
    }
    assert(pushFailure.countHost("oauth2.googleapis.com") == 1);
    assert(pushFailure.countHost("fcm.googleapis.com") == 1);
    assert(pushFailure.countHost("push.example.test") == 1);
    bool fcmBeforePush = false;
    unsigned int fcmIndex = 0, pushIndex = 0;
    for (unsigned int i = 0; i < pushFailure.requests.size(); ++i) {
        if (pushFailure.requests[i].host == "fcm.googleapis.com") fcmIndex = i;
        if (pushFailure.requests[i].host == "push.example.test") pushIndex = i;
    }
    fcmBeforePush = fcmIndex < pushIndex;
    assert(fcmBeforePush);
    unlink(secondAccount.c_str());
    cleanNotificationRuntime(directory);

    directory = root + "/both-fail";
    assert(mkdir(directory.c_str(), 0700) == 0);
    prepareBrowserSubscription(directory);
    const std::string thirdAccount = makeAccount(directory + "/service-account.json", "worker-three@example.test");
    FakeHttpsTransport bothFail;
    bothFail.oauthFailure = true;
    bothFail.failPush = true;
    setHttpsPostHook(&bothFail);
    {
        NotificationWorker worker(directory, thirdAccount, "alerts", "fake-ca-both-fail");
        assert(worker.start(&error));
        assert(worker.enqueue("Test", "both transports fail", &error));
        worker.stop();
    }
    assert(bothFail.countHost("oauth2.googleapis.com") == 1);
    assert(bothFail.countHost("push.example.test") == 1);
    unlink(thirdAccount.c_str());
    cleanNotificationRuntime(directory);

    directory = root + "/fcm-only";
    assert(mkdir(directory.c_str(), 0700) == 0);
    const std::string fourthAccount = makeAccount(directory + "/service-account.json", "worker-four@example.test");
    FakeHttpsTransport fcmOnly;
    fcmOnly.fcmResponse = "HTTP/1.1 200 OK\r\nContent-Length: 31\r\n\r\n{\"name\":\"projects/p/messages/m\"}";
    setHttpsPostHook(&fcmOnly);
    {
        NotificationWorker worker(directory, fourthAccount, "alerts", "fake-ca-fcm-only");
        assert(worker.start(&error));
        assert(worker.enqueue("Test", "FCM only", &error));
        worker.stop();
    }
    assert(fcmOnly.countHost("fcm.googleapis.com") == 1);
    assert(fcmOnly.countHost("push.example.test") == 0);
    unlink(fourthAccount.c_str());
    cleanNotificationRuntime(directory);

    directory = root + "/unconfigured";
    assert(mkdir(directory.c_str(), 0700) == 0);
    FakeHttpsTransport unconfigured;
    setHttpsPostHook(&unconfigured);
    {
        NotificationWorker worker(directory, "", "", "");
        assert(worker.start(&error));
        assert(worker.enqueue("Test", "no transports", &error));
        worker.stop();
    }
    assert(unconfigured.requests.empty());
    cleanNotificationRuntime(directory);
}
}

void runOutboundIntegrationTests() {
    char directoryName[] = "/tmp/microhad-zigbee-outbound-XXXXXX";
    char* directory = mkdtemp(directoryName);
    assert(directory != 0);
    FakeHttpsTransport transport;
    setHttpsPostHook(&transport);
    testFcmTokenCacheAndFailures(directory, &transport);
    testNotificationTransportIndependence(directory);
    setHttpsPostHook(0);
    unlink((std::string(directory) + "/account-one.json").c_str());
    unlink((std::string(directory) + "/account-two.json").c_str());
    unlink((std::string(directory) + "/account-expiring.json").c_str());
    assert(rmdir(directory) == 0);
    fprintf(stderr, "integration: FCM token caching, expiry, account change, and failures\n");
    fprintf(stderr, "integration: notification worker isolates FCM and Browser Push failures\n");
}
