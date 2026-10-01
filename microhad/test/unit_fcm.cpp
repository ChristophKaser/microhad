#include "fcm.h"
#include "https_client.h"
#include "json_util.h"

#include <cassert>
#include <cstdio>
#include <string>

static void testFcmEncodingsAndJwt() {
    std::string decoded;
    std::string error;
    FirebaseServiceAccount account;
    FirebaseServiceAccount invalid;
    std::string key;
    std::string jwt;
    std::string::size_type firstDot;
    std::string::size_type secondDot;

    assert(base64UrlEncode("f") == "Zg");
    assert(base64UrlEncode("fo") == "Zm8");
    assert(base64UrlEncode("foo") == "Zm9v");
    assert(base64UrlEncode("foob") == "Zm9vYg");
    assert(base64UrlDecode("Zg", &decoded) && decoded == "f");
    assert(base64UrlDecode("Zm8", &decoded) && decoded == "fo");
    assert(base64UrlDecode("Zm9v", &decoded) && decoded == "foo");
    assert(base64UrlDecode("Zm9vYg", &decoded) && decoded == "foob");
    assert(!base64UrlDecode("Zg=", &decoded));
    assert(!base64UrlDecode("A", &decoded));
    assert(!base64UrlDecode("Zm8!", &decoded));
    assert(makeFcmMessage("a\"b", "line\n", "slash\\\001").
           compare("{\"message\":{\"topic\":\"a\\\"b\",\"notification\":{\"title\":\"line\\n\",\"body\":\"slash\\\\\\u0001\"},\"android\":{\"priority\":\"high\"}}}") == 0);
    assert(makeFcmTokenMessage("token", "Trigger", "Entry triggered") ==
           "{\"message\":{\"token\":\"token\",\"notification\":{\"title\":\"Trigger\",\"body\":\"Entry triggered\"},\"android\":{\"priority\":\"high\"}}}");
    assert(makeFcmTokenMessage("token", "Trigger", "Entry triggered", "de.caser.garage.triggers") ==
           "{\"message\":{\"token\":\"token\",\"notification\":{\"title\":\"Trigger\",\"body\":\"Entry triggered\"},\"android\":{\"priority\":\"high\",\"notification\":{\"channel_id\":\"de.caser.garage.triggers\"}}}}");

    assert(readTextFile("test/fixtures/tls/localhost.key", &key));
    account.projectId = "project";
    account.clientEmail = "alerts@example.test";
    account.privateKeyPem = key;
    jwt = makeGoogleAccessTokenJwt(account, 1000, &error);
    firstDot = jwt.find('.'); secondDot = jwt.find('.', firstDot + 1);
    assert(firstDot != std::string::npos && secondDot != std::string::npos && secondDot + 1 < jwt.size());
    assert(base64UrlDecode(jwt.substr(0, firstDot), &decoded) && decoded == "{\"alg\":\"RS256\",\"typ\":\"JWT\"}");
    assert(base64UrlDecode(jwt.substr(firstDot + 1, secondDot - firstDot - 1), &decoded));
    assert(decoded == "{\"iss\":\"alerts@example.test\",\"scope\":\"https://www.googleapis.com/auth/firebase.messaging\",\"aud\":\"https://oauth2.googleapis.com/token\",\"iat\":1000,\"exp\":4600}");
    invalid = account; invalid.privateKeyPem = "not a PEM key";
    assert(makeGoogleAccessTokenJwt(invalid, 1000, &error).empty());
    assert(error == "cannot sign Firebase OAuth JWT");
}

static void testFirebaseServiceAccountValidation() {
    FirebaseServiceAccount account;
    std::string error;
    const std::string valid = "{\"project_id\":\"project\",\"client_email\":\"alerts@example.test\",\"private_key\":\"-----BEGIN PRIVATE KEY-----\\nkey\\n-----END PRIVATE KEY-----\\n\"}";
    assert(parseFirebaseServiceAccount(valid, &account, &error));
    assert(account.privateKeyPem == "-----BEGIN PRIVATE KEY-----\nkey\n-----END PRIVATE KEY-----\n");
    assert(!parseFirebaseServiceAccount("{}", &account, &error));
    assert(!parseFirebaseServiceAccount("{\"project_id\":\"project\",\"client_email\":\"alerts@example.test\",\"private_key\":5}", &account, &error));
    assert(!parseFirebaseServiceAccount("{\"project_id\":\"project\",\"client_email\":\"alerts@example.test\",\"private_key\":\"not a key\"}", &account, &error));
    assert(!parseFirebaseServiceAccount("{\"project_id\":\"project\",\"client_email\":\"alerts@example.test\",\"private_key\":\"\\u12zz\"}", &account, &error));
    assert(error == "Firebase service account is missing project_id, client_email, or private_key");
}

static void testHttpsUrlValidation() {
    std::string response;
    std::string error;
    const char* valid[] = {"https://push.example.test", "https://push.example.test/path", "https://push.example.test?topic=alert"};
    const char* invalid[] = {"http://push.example.test", "https://", "https:///path", "https://user@push.example.test", "https://push.example.test/#fragment", "https://push.example.test/a b"};
    for (unsigned long i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        assert(!httpsPostUrl(valid[i], "", "", "", &response, &error));
        assert(error == "a CA bundle is required for HTTPS");
    }
    for (unsigned long i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!httpsPostUrl(invalid[i], "", "", "", &response, &error));
        assert(error == "Push endpoint must use HTTPS" || error == "invalid HTTPS URL");
    }
}

void runFcmUnitTests() {
    FirebaseServiceAccount account;
    std::string error;
    fprintf(stderr, "unit: FCM service-account parsing and message encoding\n");
    assert(parseFirebaseServiceAccount("{\"project_id\":\"project\",\"client_email\":\"alerts@example.test\",\"private_key\":\"-----BEGIN PRIVATE KEY-----\\nkey\\n-----END PRIVATE KEY-----\"}", &account, &error));
    assert(!parseFirebaseServiceAccount("{\"project_id\":\"project\"}", &account, &error));
    assert(makeFcmMessage("alerts", "Window open", "Kitchen").find("\"topic\":\"alerts\"") != std::string::npos);
    assert(base64UrlEncode("foo") == "Zm9v");
    testFcmEncodingsAndJwt();
    testFirebaseServiceAccountValidation();
    testHttpsUrlValidation();
}
