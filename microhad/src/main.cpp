#include "gateway.h"
#include "http_server.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
bool running = true;

void stopHandler(int) {
    running = false;
}

std::string jsonString(const std::string& content, const char* key, const std::string& fallback) {
    std::string marker = std::string("\"") + key + "\"";
    std::string::size_type keyAt = content.find(marker);
    std::string::size_type colon;
    std::string::size_type start;
    std::string::size_type end;
    if (keyAt == std::string::npos) return fallback;
    colon = content.find(':', keyAt + marker.size());
    if (colon == std::string::npos) return fallback;
    start = content.find('"', colon + 1);
    if (start == std::string::npos) return fallback;
    end = content.find('"', start + 1);
    return end == std::string::npos ? fallback : content.substr(start + 1, end - start - 1);
}

int jsonInt(const std::string& content, const char* key, int fallback) {
    std::string marker = std::string("\"") + key + "\"";
    std::string::size_type keyAt = content.find(marker);
    std::string::size_type colon;
    if (keyAt == std::string::npos) return fallback;
    colon = content.find(':', keyAt + marker.size());
    return colon == std::string::npos ? fallback : atoi(content.c_str() + colon + 1);
}

bool readFile(const char* filename, std::string* content) {
    std::ifstream input(filename);
    if (!input) return false;
    content->assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return true;
}
}

int main(int argc, char** argv) {
    const char* configPath = "/tuya/native/config.json";
    std::string config;
    std::string error;
    std::string serialDevice;
    std::string runtimeDirectory;
    int serialBaud;
    int httpPort;
    Gateway* gateway;
    HttpServer* server;

    if (argc == 3 && strcmp(argv[1], "--config") == 0) configPath = argv[2];
    else if (argc != 1) {
        fprintf(stderr, "usage: %s [--config /protected/path/config.json]\n", argv[0]);
        return 2;
    }
    if (!readFile(configPath, &config)) {
        fprintf(stderr, "cannot read configuration %s: %s\n", configPath, strerror(errno));
        return 2;
    }
    serialDevice = jsonString(config, "serial_device", "/dev/ttyS1");
    serialBaud = jsonInt(config, "serial_baud", 115200);
    runtimeDirectory = jsonString(config, "runtime_dir", "/tuya/native");
    httpPort = jsonInt(config, "https_port", jsonInt(config, "http_port", 8080));
    gateway = new Gateway(serialDevice, serialBaud, runtimeDirectory,
                          jsonString(config, "firebase_service_account", ""),
                          jsonString(config, "firebase_topic", "zigbee_alerts"),
                          jsonString(config, "ca_bundle", ""));
    if (!gateway->start(&error)) {
        fprintf(stderr, "gateway startup failed: %s\n", error.c_str());
        delete gateway;
        return 1;
    }
    server = new HttpServer(httpPort, gateway, jsonString(config, "tls_certificate", ""),
                            jsonString(config, "tls_private_key", ""));
    if (!server->start(&error)) {
        fprintf(stderr, "web startup failed: %s\n", error.c_str());
        delete server;
        delete gateway;
        return 1;
    }
    signal(SIGINT, stopHandler);
    signal(SIGTERM, stopHandler);
    fprintf(stderr, "microhad-zigbee-native: serial=%s http=%d\n", serialDevice.c_str(), httpPort);
    while (running) {
        gateway->poll();
        server->poll();
        usleep(20000);
    }
    delete server;
    delete gateway;
    return 0;
}
