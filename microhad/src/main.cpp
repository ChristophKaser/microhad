#include "gateway.h"
#include "gateway_runner.h"
#include "notification_worker.h"
#include "http_server.h"
#include "gateway_config.h"
#include "companion_discovery.h"
#include "external_http_device.h"
#include "thread_name.h"
#include "matter_worker.h"
#include "script_benchmark.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <syslog.h>
#include <unistd.h>

namespace {
bool running = true;

void stopHandler(int) {
    running = false;
}

}

int main(int argc, char** argv) {
    openlog("gateway", 0, LOG_USER);
    setCurrentThreadName("gateway-main");
    if (argc >= 4 && std::string(argv[1]) == "--script-benchmark") {
        char* end = 0;
        unsigned long runs = 1;
        if (argc == 6 && std::string(argv[4]) == "--runs") runs = strtoul(argv[5], &end, 10);
        else if (argc != 4) { fprintf(stderr, "usage: microhad --script-benchmark lua|quickjs SCRIPT [--runs N]\n"); return 2; }
        if (argc == 6 && (end == argv[5] || *end != '\0')) { fprintf(stderr, "invalid benchmark run count\n"); return 2; }
        std::string result;
        std::string benchmarkError;
        if (!runScriptBenchmark(argv[2], argv[3], runs, &result, &benchmarkError)) {
            fprintf(stderr, "script benchmark failed: %s\n", benchmarkError.c_str()); return 1;
        }
        printf("%s\n", result.c_str()); return 0;
    }
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
    ExternalHttpDeviceWorker* shellyIntegration;

    const bool printConfig = argc > 2 && std::string(argv[1]) == "--print-config";
    std::string configField;
    std::map<std::string, std::string> configuredValues;
    if (printConfig) { configField = argv[2]; argc -= 2; argv += 2; }
    if (!selectGatewayConfigPath(argc, const_cast<const char* const*>(argv), &configPath, &error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    if (!loadGatewayConfig(configPath, &config, &error, printConfig ? &configuredValues : 0)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    if (printConfig) {
        const std::string& value = configuredValues[configField];
        fwrite(value.data(), 1, value.size(), stdout);
        putchar('\n');
        return 0;
    }
    serialDevice = config.serialDevice;
    serialBaud = static_cast<int>(config.serialBaud);
    runtimeDirectory = config.runtimeDirectory;
    httpPort = static_cast<int>(config.httpPort);
    httpsPort = static_cast<int>(config.httpsPort);
    std::string firebaseServiceAccount = config.firebaseServiceAccount;
    std::string firebaseTopic = config.firebaseTopic;
    std::string caBundle = config.caBundle;
    signal(SIGPIPE, SIG_IGN);
    notificationWorker = new NotificationWorker(runtimeDirectory, firebaseServiceAccount, firebaseTopic, caBundle);
    /* LinuxThreads creates its manager task on the first pthread_create and
       inherits the caller's task name. Give that task its own diagnostic name. */
    setCurrentThreadName("pthread-manager");
    bool notificationStarted = notificationWorker->start(&error);
    setCurrentThreadName("gateway-main");
    if (!notificationStarted) {
        fprintf(stderr, "notification worker startup failed: %s\n", error.c_str());
        delete notificationWorker;
        return 1;
    }
    gateway = new Gateway(serialDevice, serialBaud, runtimeDirectory, firebaseServiceAccount, firebaseTopic, caBundle,
                          notificationWorker, "/tmp/microhad-history.bin", runtimeDirectory + "/history.values");
#if MICROHAD_ZIGGURAT_ENABLED
    gateway->useZiggurat(config.zigguratSocket);
#endif
    gateway->configureBroadcastQuorum(static_cast<unsigned int>(config.broadcastPassiveAckQuorum));
    gateway->configureMatterCommissioningRegion(config.matterCountryCode, config.matterRegulatoryLocation);
    gatewayRunner = new GatewayRunner(gateway);
    MatterWorker matterWorker(gatewayRunner);
    shellyIntegration = new ExternalHttpDeviceWorker(runtimeDirectory, gatewayRunner);
    gateway->setShellyTransport(shellyIntegration);
    if (!gatewayRunner->start(&error)) {
        fprintf(stderr, "gateway startup failed: %s\n", error.c_str());
        delete shellyIntegration;
        matterWorker.stop();
        delete gatewayRunner;
        delete notificationWorker;
        return 1;
    }
    if (!shellyIntegration->start(&error)) {
        fprintf(stderr, "Shelly integration startup failed: %s\n", error.c_str());
        delete shellyIntegration;
        matterWorker.stop();
        delete gatewayRunner;
        delete notificationWorker;
        return 1;
    }
#if MICROHAD_MATTER_ENABLED
    if (!matterWorker.start(&error)) {
        fprintf(stderr, "Matter worker startup failed: %s\n", error.c_str());
        delete shellyIntegration; matterWorker.stop(); delete gatewayRunner; delete notificationWorker;
        return 1;
    }
#endif
    server = new HttpServer(httpPort, gatewayRunner, runtimeDirectory + "/webui", "", "", shellyIntegration, notificationWorker, &matterWorker, config.companionJson());
    if (!server->start(&error)) {
        fprintf(stderr, "web startup failed: %s\n", error.c_str());
        delete server;
        delete shellyIntegration;
        matterWorker.stop();
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
        } else {
            httpsServer = new HttpServer(httpsPort, gatewayRunner, runtimeDirectory + "/webui", certificatePath, privateKeyPath, shellyIntegration, notificationWorker, &matterWorker, config.companionJson());
            if (!httpsServer->start(&error)) {
                fprintf(stderr, "HTTPS disabled: %s\n", error.c_str());
                delete httpsServer;
                httpsServer = 0;
            }
        }
    }
    signal(SIGINT, stopHandler);
    signal(SIGTERM, stopHandler);
    fprintf(stderr, "microhad: serial=%s http=%d https=%d\n", serialDevice.c_str(), httpPort,
            httpsServer == 0 ? 0 : httpsPort);
    CompanionDiscovery discovery(static_cast<unsigned short>(httpPort));
    while (running) {
        if (config.companionDiscovery) discovery.poll();
        server->poll();
        if (httpsServer != 0) httpsServer->poll();
        usleep(20000);
    }
    delete server;
    delete httpsServer;
    delete shellyIntegration;
    matterWorker.stop();
    delete gatewayRunner;
    delete notificationWorker;
    return 0;
}
