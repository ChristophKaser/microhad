#include "gateway.h"
#include "gateway_runner.h"
#include "notification_worker.h"
#include "http_server.h"
#include "json_util.h"

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

}

int main(int argc, char** argv) {
    const char* configPath = "/tuya/native/config.json";
    std::string config;
    std::string error;
    std::string serialDevice;
    std::string runtimeDirectory;
    int serialBaud;
    int httpPort;
    int httpsPort;
    Gateway* gateway;
    GatewayRunner* gatewayRunner;
    NotificationWorker* notificationWorker;
    HttpServer* server;

    if (argc == 3 && strcmp(argv[1], "--config") == 0) configPath = argv[2];
    else if (argc != 1) {
        fprintf(stderr, "usage: %s [--config /protected/path/config.json]\n", argv[0]);
        return 2;
    }
    if (!readTextFile(configPath, &config)) {
        fprintf(stderr, "cannot read configuration %s: %s\n", configPath, strerror(errno));
        return 2;
    }
    long setting;
    serialDevice = "/dev/ttyS1"; jsonStringField(config, "serial_device", &serialDevice);
    serialBaud = jsonLongField(config, "serial_baud", &setting) ? static_cast<int>(setting) : 115200;
    runtimeDirectory = "/tuya/native"; jsonStringField(config, "runtime_dir", &runtimeDirectory);
    httpPort = jsonLongField(config, "http_port", &setting) ? static_cast<int>(setting) : 80;
    httpsPort = jsonLongField(config, "https_port", &setting) ? static_cast<int>(setting) : 443;
    std::string firebaseServiceAccount; jsonStringField(config, "firebase_service_account", &firebaseServiceAccount);
    std::string firebaseTopic = "zigbee_alerts"; jsonStringField(config, "firebase_topic", &firebaseTopic);
    std::string caBundle; jsonStringField(config, "ca_bundle", &caBundle);
    notificationWorker = new NotificationWorker(runtimeDirectory, firebaseServiceAccount, firebaseTopic, caBundle);
    if (!notificationWorker->start(&error)) {
        fprintf(stderr, "notification worker startup failed: %s\n", error.c_str());
        delete notificationWorker;
        return 1;
    }
    gateway = new Gateway(serialDevice, serialBaud, runtimeDirectory, firebaseServiceAccount, firebaseTopic, caBundle,
                          notificationWorker);
    gatewayRunner = new GatewayRunner(gateway);
    if (!gatewayRunner->start(&error)) {
        fprintf(stderr, "gateway startup failed: %s\n", error.c_str());
        delete gatewayRunner;
        delete notificationWorker;
        return 1;
    }
    server = new HttpServer(httpPort, gatewayRunner, runtimeDirectory + "/webui/index.html", "", "");
    if (!server->start(&error)) {
        fprintf(stderr, "web startup failed: %s\n", error.c_str());
        delete server;
        delete gatewayRunner;
        delete notificationWorker;
        return 1;
    }
    HttpServer* httpsServer = 0;
    std::string certificatePath; jsonStringField(config, "tls_certificate", &certificatePath);
    std::string privateKeyPath; jsonStringField(config, "tls_private_key", &privateKeyPath);
    if (!certificatePath.empty() || !privateKeyPath.empty()) {
        if (certificatePath.empty() || privateKeyPath.empty()) {
            fprintf(stderr, "both tls_certificate and tls_private_key are required for HTTPS\n");
            delete server;
            delete gateway;
            return 2;
        }
        httpsServer = new HttpServer(httpsPort, gatewayRunner, runtimeDirectory + "/webui/index.html", certificatePath, privateKeyPath);
        if (!httpsServer->start(&error)) {
            fprintf(stderr, "HTTPS startup failed: %s\n", error.c_str());
            delete httpsServer;
            delete server;
            delete gatewayRunner;
            delete notificationWorker;
            return 1;
        }
    }
    signal(SIGINT, stopHandler);
    signal(SIGTERM, stopHandler);
    /* A browser may close a keep-alive/TLS socket before a response write.
       Treat that as a normal write failure instead of terminating the gateway. */
    signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "microhad-zigbee-native: serial=%s http=%d https=%d\n", serialDevice.c_str(), httpPort,
            httpsServer == 0 ? 0 : httpsPort);
    while (running) {
        server->poll();
        if (httpsServer != 0) httpsServer->poll();
        usleep(20000);
    }
    delete server;
    delete httpsServer;
    delete gatewayRunner;
    delete notificationWorker;
    return 0;
}
