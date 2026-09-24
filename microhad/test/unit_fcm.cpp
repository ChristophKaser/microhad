#include "fcm.h"
#include "json_util.h"

#include <cassert>
#include <cstdio>
#include <string>

void runFcmUnitTests() {
    FirebaseServiceAccount account;
    std::string error;
    fprintf(stderr, "unit: FCM service-account parsing and message encoding\n");
    assert(parseFirebaseServiceAccount("{\"project_id\":\"project\",\"client_email\":\"alerts@example.test\",\"private_key\":\"key\"}", &account, &error));
    assert(!parseFirebaseServiceAccount("{\"project_id\":\"project\"}", &account, &error));
    assert(makeFcmMessage("alerts", "Window open", "Kitchen").find("\"topic\":\"alerts\"") != std::string::npos);
    assert(base64UrlEncode("foo") == "Zm9v");
}
