#include "gateway_config.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

void runGatewayConfigUnitTests() {
    std::string path;
    std::string error;
    const char* defaultArguments[] = {"microhad-zigbee-native"};
    assert(selectGatewayConfigPath(1, defaultArguments, &path, &error));
    assert(path == "/tuya/native/config.json");
    GatewayConfig defaults;
    assert(defaults.serialDevice == "/dev/ttyS1" && defaults.serialBaud == 115200);
    assert(defaults.runtimeDirectory == "/tuya/native" && defaults.httpPort == 80 && defaults.httpsPort == 443);
    assert(defaults.firebaseTopic == "zigbee_alerts" && defaults.firebaseServiceAccount.empty());

    char filename[] = "/tmp/microhad-zigbee-config-valid-XXXXXX";
    int fd = mkstemp(filename);
    assert(fd >= 0);
    const std::string document = "{\"serial_device\":\"/dev/test-radio\",\"http_port\":8088,\"runtime_dir\":\"/tmp/test-runtime\"}";
    assert(write(fd, document.data(), document.size()) == static_cast<ssize_t>(document.size()));
    close(fd);
    const char* customArguments[] = {"microhad-zigbee-native", "--config", filename};
    assert(selectGatewayConfigPath(3, customArguments, &path, &error) && path == filename);
    GatewayConfig selected;
    assert(loadGatewayConfig(path, &selected, &error));
    assert(selected.serialDevice == "/dev/test-radio" && selected.httpPort == 8088);
    assert(selected.runtimeDirectory == "/tmp/test-runtime");
    assert(selected.serialBaud == 115200 && selected.httpsPort == 443);
    assert(selected.firebaseTopic == "zigbee_alerts" && selected.firebaseServiceAccount.empty());
    unlink(filename);
    fprintf(stderr, "unit: CLI config selection and defaults\n");
}
