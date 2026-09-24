#include "fcm.h"
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

void runFcmUnitTests() {
    FirebaseServiceAccount account;
    std::string error;
    fprintf(stderr, "unit: FCM service-account parsing and message encoding\n");
    assert(parseFirebaseServiceAccount("{\"project_id\":\"project\",\"client_email\":\"alerts@example.test\",\"private_key\":\"key\"}", &account, &error));
    assert(!parseFirebaseServiceAccount("{\"project_id\":\"project\"}", &account, &error));
    assert(makeFcmMessage("alerts", "Window open", "Kitchen").find("\"topic\":\"alerts\"") != std::string::npos);
    assert(base64UrlEncode("foo") == "Zm9v");
    testFcmEncodingsAndJwt();
}
