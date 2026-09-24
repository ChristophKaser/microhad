#include "fcm.h"
#include "https_client.h"
#include "json_util.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
struct HttpsRequest {
    std::string host;
    std::string path;
    std::string headers;
    std::string body;
};

class FakeHttpsTransport : public HttpsPostHook {
public:
    FakeHttpsTransport() : oauthFailure(false), malformedOAuth(false), expiresIn(3600) {}
    bool oauthFailure;
    bool malformedOAuth;
    long expiresIn;
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
        *error = "unexpected fake host";
        return false;
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
}

void runOutboundIntegrationTests() {
    char directoryName[] = "/tmp/microhad-zigbee-outbound-XXXXXX";
    char* directory = mkdtemp(directoryName);
    assert(directory != 0);
    FakeHttpsTransport transport;
    setHttpsPostHook(&transport);
    testFcmTokenCacheAndFailures(directory, &transport);
    setHttpsPostHook(0);
    unlink((std::string(directory) + "/account-one.json").c_str());
    unlink((std::string(directory) + "/account-two.json").c_str());
    unlink((std::string(directory) + "/account-expiring.json").c_str());
    assert(rmdir(directory) == 0);
    fprintf(stderr, "integration: FCM token caching, expiry, account change, and failures\n");
}
