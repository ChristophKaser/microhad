#include "gateway.h"
#include "gateway_runner.h"
#include "notification_worker.h"
#include "http_server.h"
#include "gateway_config.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {
bool running = true;

void stopHandler(int) {
    running = false;
}

}

int main(int argc, char** argv) {
    std::string configPath;
    std::string error;
    GatewayConfig config;
    std::string serialDevice;
    std::string runtimeDirectory;
    int serialBaud;
    int httpPort;
    int httpsPort;
    Gateway* gateway;
    GatewayRunner* gatewayRunner;
    NotificationWorker* notificationWorker;
    HttpServer* server;

    if (!selectGatewayConfigPath(argc, const_cast<const char* const*>(argv), &configPath, &error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    if (!loadGatewayConfig(configPath, &config, &error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    serialDevice = config.serialDevice;
    serialBaud = static_cast<int>(config.serialBaud);
    runtimeDirectory = config.runtimeDirectory;
    httpPort = static_cast<int>(config.httpPort);
    httpsPort = static_cast<int>(config.httpsPort);
    std::string firebaseServiceAccount = config.firebaseServiceAccount;
    std::string firebaseTopic = config.firebaseTopic;
    std::string caBundle = config.caBundle;
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
    server = new HttpServer(httpPort, gatewayRunner, runtimeDirectory + "/webui", "", "");
    if (!server->start(&error)) {
        fprintf(stderr, "web startup failed: %s\n", error.c_str());
        delete server;
        delete gatewayRunner;
        delete notificationWorker;
        return 1;
    }
    HttpServer* httpsServer = 0;
    std::string certificatePath = config.tlsCertificate;
    std::string privateKeyPath = config.tlsPrivateKey;
    if (!certificatePath.empty() || !privateKeyPath.empty()) {
        if (certificatePath.empty() || privateKeyPath.empty()) {
            fprintf(stderr, "both tls_certificate and tls_private_key are required for HTTPS\n");
            delete server;
            delete gateway;
            return 2;
        }
        httpsServer = new HttpServer(httpsPort, gatewayRunner, runtimeDirectory + "/webui", certificatePath, privateKeyPath);
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
