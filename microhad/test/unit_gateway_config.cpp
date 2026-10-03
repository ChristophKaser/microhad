#include "gateway_config.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void replaceFileContents(const std::string& filename, const std::string& contents) {
    int fd = open(filename.c_str(), O_WRONLY | O_TRUNC);
    assert(fd >= 0);
    size_t written = 0;
    while (written < contents.size()) {
        ssize_t count = write(fd, contents.data() + written, contents.size() - written);
        assert(count > 0);
        written += static_cast<size_t>(count);
    }
    close(fd);
}

std::string runInvalidConfigProcess(const std::string& filename, int* exitCode) {
    int output[2];
    char buffer[512];
    ssize_t count;
    std::string captured;
    int status;
    assert(pipe(output) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(output[0]);
        assert(dup2(output[1], STDERR_FILENO) == STDERR_FILENO);
        close(output[1]);
        execl(MICROHAD_TEST_BINARY, "microhad-zigbee-native", "--config", filename.c_str(), static_cast<char*>(0));
        _exit(127);
    }
    close(output[1]);
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0)
        captured.append(buffer, static_cast<unsigned long>(count));
    close(output[0]);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    *exitCode = WEXITSTATUS(status);
    return captured;
}
}

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
    fprintf(stderr, "unit: CLI config selection and defaults\n");

    const std::string invalidDocuments[] = {
        "", "{", "[]", "{\"serial_device\":}", "{\"serial_device\":42}",
        "{\"http_port\":\"8080\"}", "{\"serial_baud\":true}",
        "{\"runtime_dir\":\"/tmp/x\",}", "{\"runtime_dir\":\"/tmp/x\"} trailing"
    };
    unsigned long i;
    for (i = 0; i < sizeof(invalidDocuments) / sizeof(invalidDocuments[0]); ++i) {
        replaceFileContents(filename, invalidDocuments[i]);
        GatewayConfig invalid;
        assert(!loadGatewayConfig(filename, &invalid, &error) && !error.empty());
    }
    const std::string invalidSettings[] = {
        "{\"serial_baud\":2147483648}", "{\"serial_baud\":9600}",
        "{\"http_port\":0}", "{\"http_port\":65536}", "{\"https_port\":-1}",
        "{\"http_port\":8080,\"https_port\":8080,\"tls_certificate\":\"cert\",\"tls_private_key\":\"key\"}",
        "{\"tls_certificate\":\"cert\"}", "{\"tls_private_key\":\"key\"}",
        "{\"runtime_dir\":\"\"}"
    };
    for (i = 0; i < sizeof(invalidSettings) / sizeof(invalidSettings[0]); ++i) {
        replaceFileContents(filename, invalidSettings[i]);
        GatewayConfig invalid;
        assert(!loadGatewayConfig(filename, &invalid, &error) && !error.empty());
    }
    assert(!loadGatewayConfig(std::string(filename) + ".missing", &selected, &error));

    char directoryName[] = "/tmp/microhad-zigbee-config-no-start-XXXXXX";
    char* directoryPath = mkdtemp(directoryName);
    assert(directoryPath != 0);
    std::string runtimePath = std::string(directoryPath) + "/must-not-be-created";
    replaceFileContents(filename, std::string("{\"runtime_dir\":\"") + runtimePath + "\",\"http_port\":\"80\"}");
    int exitCode = -1;
    std::string output = runInvalidConfigProcess(filename, &exitCode);
    assert(exitCode == 2 && output.find("configuration field must be an integer: http_port") != std::string::npos);
    assert(access(runtimePath.c_str(), F_OK) != 0);
    replaceFileContents(filename, std::string("{\"runtime_dir\":\"") + runtimePath + "\",\"http_port\":65536}");
    output = runInvalidConfigProcess(filename, &exitCode);
    assert(exitCode == 2 && output.find("listener ports must be between 1 and 65535") != std::string::npos);
    assert(access(runtimePath.c_str(), F_OK) != 0);
    unlink(filename);
    rmdir(directoryPath);
    fprintf(stderr, "unit: malformed and wrong-type configuration rejected before startup\n");
    fprintf(stderr, "unit: out-of-range gateway settings rejected before startup\n");
}
