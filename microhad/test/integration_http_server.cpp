#include "gateway.h"
#include "gateway_runner.h"
#include "gateway_integration_peer.h"
#include "http_server.h"
#include "notification_worker.h"
#include "fcm.h"
#include "storage.h"
#include "sensor_history.h"
#include "external_http_device.h"
#include "json_util.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#pragma GCC diagnostic pop

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <fstream>
#include <sstream>
#include <string>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::string makeDirectory() {
    char name[] = "/tmp/microhad-zigbee-integration-XXXXXX";
    char* directory = mkdtemp(name);
    assert(directory != 0);
    return directory;
}

void writeText(const std::string& name, const std::string& text) {
    std::ofstream output(name.c_str());
    assert(output);
    output << text;
    output.close();
    assert(output);
}

void removeDirectory(const std::string& directory) {
    unlink((directory + "/webui/index.html").c_str());
    unlink((directory + "/webui/sw.js").c_str());
    unlink((directory + "/webui/assets/app.js").c_str());
    unlink((directory + "/webui/assets/app.css").c_str());
    rmdir((directory + "/webui/assets").c_str());
    rmdir((directory + "/webui").c_str());
    unlink((directory + "/notifications.json").c_str());
    unlink((directory + "/vapid-public.key").c_str());
    unlink((directory + "/vapid-private.key").c_str());
    unlink((directory + "/web-push-subscriptions.ndjson").c_str());
    unlink((directory + "/push-devices.ndjson").c_str());
    unlink((directory + "/scenes.conf").c_str());
    unlink((directory + "/dashboard.json").c_str());
    unlink((directory + "/dashboard.json.tmp").c_str());
    unlink((directory + "/devices.json").c_str());
    unlink((directory + "/history.bin").c_str());
    unlink((directory + "/shelly-devices.jsonl").c_str());
    rmdir(directory.c_str());
}

int connectLoopback(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    struct timeval timeout;
    assert(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0);
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    return fd;
}

std::string readAll(int fd) {
    std::string output;
    char buffer[1024];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof(buffer))) > 0) output.append(buffer, static_cast<unsigned long>(count));
    return output;
}

std::string plainRequest(int port, const std::string& request) {
    int fd = connectLoopback(port);
    assert(write(fd, request.data(), request.size()) == static_cast<ssize_t>(request.size()));
    std::string response = readAll(fd);
    close(fd);
    return response;
}

std::string plainRequestParts(int port, const std::vector<std::string>& parts, bool endWrite) {
    int fd = connectLoopback(port);
    for (unsigned long i = 0; i < parts.size(); ++i)
        assert(write(fd, parts[i].data(), parts[i].size()) == static_cast<ssize_t>(parts[i].size()));
    if (endWrite) assert(shutdown(fd, SHUT_WR) == 0);
    std::string response = readAll(fd);
    close(fd);
    return response;
}

std::string tlsRequest(int port, const std::string& request) {
    mbedtls_net_context socket;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_ctr_drbg_context random;
    mbedtls_entropy_context entropy;
    std::ostringstream portText;
    std::string response;
    char buffer[1024];
    int result;
    mbedtls_net_init(&socket); mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&config);
    mbedtls_x509_crt_init(&certificate); mbedtls_ctr_drbg_init(&random); mbedtls_entropy_init(&entropy);
    portText << port;
    result = mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy,
                                   reinterpret_cast<const unsigned char*>("integration"), 11);
    if (result == 0) result = mbedtls_x509_crt_parse_file(&certificate, "test/fixtures/tls/localhost.crt");
    if (result == 0) result = mbedtls_net_connect(&socket, "127.0.0.1", portText.str().c_str(), MBEDTLS_NET_PROTO_TCP);
    if (result == 0) result = mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result == 0) {
        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&config, &certificate, 0);
        mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &random);
        result = mbedtls_ssl_setup(&ssl, &config);
    }
    if (result == 0) result = mbedtls_ssl_set_hostname(&ssl, "localhost");
    if (result == 0) {
        mbedtls_ssl_set_bio(&ssl, &socket, mbedtls_net_send, mbedtls_net_recv, 0);
        while ((result = mbedtls_ssl_handshake(&ssl)) != 0 &&
               (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)) {}
    }
    assert(result == 0 && mbedtls_ssl_get_verify_result(&ssl) == 0);
    unsigned long offset = 0;
    while (offset < request.size()) {
        result = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(request.data()) + offset, request.size() - offset);
        assert(result > 0);
        offset += static_cast<unsigned long>(result);
    }
    while ((result = mbedtls_ssl_read(&ssl, reinterpret_cast<unsigned char*>(buffer), sizeof(buffer))) > 0)
        response.append(buffer, static_cast<unsigned long>(result));
    assert(result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY);
    mbedtls_ssl_close_notify(&ssl); mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&config);
    mbedtls_net_free(&socket); mbedtls_x509_crt_free(&certificate); mbedtls_ctr_drbg_free(&random); mbedtls_entropy_free(&entropy);
    return response;
}

void testHttpQueuePressureAndBindFailure(HttpServer* server, GatewayRunner* runner, const std::string& dashboardPath) {
    int blocker = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    socklen_t addressLength = sizeof(address);
    std::string error;
    assert(blocker >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = 0;
    assert(bind(blocker, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0);
    assert(listen(blocker, 2) == 0);
    assert(getsockname(blocker, reinterpret_cast<struct sockaddr*>(&address), &addressLength) == 0);
    HttpServer bindFailure(ntohs(address.sin_port), runner, dashboardPath, "", "");
    assert(!bindFailure.start(&error) && !error.empty());
    close(blocker);

    std::vector<int> delayed;
    std::vector<int> pressure;
    const std::string partial = "GET /api/status HTTP/1.1\r\nHost: localhost\r\n";
    const std::string complete = "GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n";
    unsigned int i;
    for (i = 0; i < 2; ++i) {
        int fd = connectLoopback(server->boundPort());
        assert(write(fd, partial.data(), partial.size()) == static_cast<ssize_t>(partial.size()));
        delayed.push_back(fd);
    }
    usleep(50000);
    for (i = 0; i < 24; ++i) {
        int fd = connectLoopback(server->boundPort());
        ssize_t sent = write(fd, complete.data(), complete.size());
        assert(sent == static_cast<ssize_t>(complete.size()) ||
               (sent < 0 && (errno == EPIPE || errno == ECONNRESET)));
        pressure.push_back(fd);
    }
    for (i = 0; i < delayed.size(); ++i) {
        struct linger reset;
        reset.l_onoff = 1; reset.l_linger = 0;
        setsockopt(delayed[i], SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        close(delayed[i]);
    }
    unsigned int responses = 0;
    for (i = 0; i < pressure.size(); ++i) {
        std::string result = readAll(pressure[i]);
        if (result.find("HTTP/1.1 200 OK\r\n") == 0) ++responses;
        close(pressure[i]);
    }
    assert(responses > 0 && responses < pressure.size());
    std::string recovery = plainRequest(server->boundPort(), complete);
    assert(recovery.find("HTTP/1.1 200 OK\r\n") == 0);
    fprintf(stderr, "integration: http survives worker queue pressure and peer closes\n");
}

void tlsAbortAfterRequest(int port) {
    mbedtls_net_context socket;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_ctr_drbg_context random;
    mbedtls_entropy_context entropy;
    std::ostringstream portText;
    const std::string request = "GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n";
    unsigned long offset = 0;
    int result;
    struct linger reset;
    mbedtls_net_init(&socket); mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&config);
    mbedtls_x509_crt_init(&certificate); mbedtls_ctr_drbg_init(&random); mbedtls_entropy_init(&entropy);
    portText << port;
    result = mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy,
                                   reinterpret_cast<const unsigned char*>("abort-integration"), 18);
    if (result == 0) result = mbedtls_x509_crt_parse_file(&certificate, "test/fixtures/tls/localhost.crt");
    if (result == 0) result = mbedtls_net_connect(&socket, "127.0.0.1", portText.str().c_str(), MBEDTLS_NET_PROTO_TCP);
    if (result == 0) result = mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result == 0) {
        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&config, &certificate, 0);
        mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &random);
        result = mbedtls_ssl_setup(&ssl, &config);
    }
    if (result == 0) result = mbedtls_ssl_set_hostname(&ssl, "localhost");
    if (result == 0) {
        mbedtls_ssl_set_bio(&ssl, &socket, mbedtls_net_send, mbedtls_net_recv, 0);
        while ((result = mbedtls_ssl_handshake(&ssl)) != 0 &&
               (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)) {}
    }
    assert(result == 0 && mbedtls_ssl_get_verify_result(&ssl) == 0);
    while (offset < request.size()) {
        result = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(request.data()) + offset, request.size() - offset);
        assert(result > 0);
        offset += static_cast<unsigned long>(result);
    }
    reset.l_onoff = 1; reset.l_linger = 0;
    assert(setsockopt(socket.fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    close(socket.fd); socket.fd = -1;
    mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&config); mbedtls_net_free(&socket);
    mbedtls_x509_crt_free(&certificate); mbedtls_ctr_drbg_free(&random); mbedtls_entropy_free(&entropy);
}

void failTlsHandshake(int port) {
    int fd = connectLoopback(port);
    const char garbage[] = "not a TLS record";
    assert(write(fd, garbage, sizeof(garbage) - 1) == static_cast<ssize_t>(sizeof(garbage) - 1));
    assert(shutdown(fd, SHUT_WR) == 0);
    readAll(fd);
    close(fd);
}

std::string post(const std::string& path, const std::string& body) {
    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " << body.size()
            << "\r\n\r\n" << body;
    return request.str();
}

std::string subscription(const std::string& endpoint) {
    std::string publicKey(65, '\0');
    std::string auth(16, '\0');
    publicKey[0] = 4;
    return "{\"endpoint\":\"" + endpoint + "\",\"keys\":{\"p256dh\":\"" +
           base64UrlEncode(publicKey) + "\",\"auth\":\"" + base64UrlEncode(auth) + "\"}}";
}

void expectStatus(const char* name, const std::string& response, int code) {
    std::ostringstream expected;
    expected << "HTTP/1.1 " << code;
    fprintf(stderr, "integration: %s\n", name);
    assert(response.find(expected.str()) == 0);
}

void testServers() {
    void (*oldSigpipe)(int) = signal(SIGPIPE, SIG_IGN);
    std::string directory = makeDirectory();
    std::string error;
    std::string response;
    int inbound[2], outbound[2];
    assert(pipe(inbound) == 0 && pipe(outbound) == 0);
    assert(ensureDirectory(directory + "/webui", &error));
    writeText(directory + "/webui/index.html", "<html>integration dashboard</html>");
    assert(ensureDirectory(directory + "/webui/assets", &error));
    writeText(directory + "/webui/assets/app.js", "console.log(\"dashboard\")");
    writeText(directory + "/webui/assets/app.css", "body{color:blue}");
    writeText(directory + "/webui/sw.js", "self.addEventListener(\"push\", function() {});");
    NotificationWorker notifications(directory, "", "", "");
    assert(notifications.start(&error));
    SensorHistory history(directory + "/history.bin");
    assert(history.append("0x00124b0000000042", SensorHistory::Temperature, 2245, 1700000000));
    Gateway* gateway = new Gateway("/dev/null", 115200, directory, "", "", "", &notifications, directory + "/history.bin");
    GatewayIntegrationTestPeer::attachSerial(gateway, inbound[0]);
    GatewayRunner runner(gateway);
    ExternalHttpDeviceWorker shelly(directory, &runner);
    gateway->setShellyTransport(&shelly);
    assert(runner.start(&error));
    assert(shelly.start(&error));
    GatewayIntegrationTestPeer::prepareHttpDevice(gateway);

    HttpServer http(0, &runner, directory + "/webui", "", "", &shelly, &notifications);
    HttpServer https(0, &runner, directory + "/webui", "test/fixtures/tls/localhost.crt", "test/fixtures/tls/localhost.key", &shelly, &notifications);
    if (!http.start(&error)) { fprintf(stderr, "HTTP start failed: %s\n", error.c_str()); assert(false); }
    if (!https.start(&error)) { fprintf(stderr, "HTTPS start failed: %s\n", error.c_str()); assert(false); }
    assert(http.boundPort() > 0 && https.boundPort() > 0);

    HttpServer missingCertificate(0, &runner, directory + "/webui", directory + "/missing.crt",
                                  "test/fixtures/tls/localhost.key");
    assert(!missingCertificate.start(&error) && error.find("TLS certificate or key") != std::string::npos);
    HttpServer invalidPrivateKey(0, &runner, directory + "/webui", "test/fixtures/tls/localhost.crt", "/dev/null");
    assert(!invalidPrivateKey.start(&error) && error.find("TLS certificate or key") != std::string::npos);
    testHttpQueuePressureAndBindFailure(&http, &runner, directory + "/webui");

    response = tlsRequest(https.boundPort(), "INVALID\r\nHost: localhost\r\n\r\n");
    expectStatus("https reject malformed encrypted request", response, 400);
    response = tlsRequest(https.boundPort(), "POST /api/join HTTP/1.1\r\nHost: localhost\r\nContent-Length: 20000\r\n\r\n");
    expectStatus("https reject oversized encrypted request", response, 413);
    failTlsHandshake(https.boundPort());
    tlsAbortAfterRequest(https.boundPort());

    response = plainRequest(http.boundPort(), "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http dashboard", response, 200);
    assert(response.find("Content-Type: text/html; charset=utf-8\r\n") != std::string::npos && response.find("<html>integration dashboard</html>") != std::string::npos);
    response = plainRequest(http.boundPort(), "GET /sw.js HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http service worker", response, 200);
    assert(response.find("Content-Type: application/javascript; charset=utf-8\r\n") != std::string::npos && response.find("addEventListener") != std::string::npos);
    response = plainRequest(http.boundPort(), "GET /assets/app.js HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http javascript asset", response, 200);
    assert(response.find("Content-Type: application/javascript; charset=utf-8\r\n") != std::string::npos && response.find("console.log") != std::string::npos);
    response = plainRequest(http.boundPort(), "GET /assets/app.css HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http css asset", response, 200);
    assert(response.find("Content-Type: text/css; charset=utf-8\r\n") != std::string::npos && response.find("body{color:blue}") != std::string::npos);
    expectStatus("http missing asset", plainRequest(http.boundPort(), "GET /assets/missing.js HTTP/1.1\r\nHost: localhost\r\n\r\n"), 404);
    expectStatus("http traversal", plainRequest(http.boundPort(), "GET /assets/../index.html HTTP/1.1\r\nHost: localhost\r\n\r\n"), 404);
    expectStatus("http encoded traversal", plainRequest(http.boundPort(), "GET /assets/%2e%2e/index.html HTTP/1.1\r\nHost: localhost\r\n\r\n"), 404);
    response = plainRequest(http.boundPort(), "GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http status", response, 200);
    assert(response.find("Content-Type: application/json\r\n") != std::string::npos && response.find("\"online\":") != std::string::npos && response.find("\"devices\":") != std::string::npos);
    response = plainRequest(http.boundPort(), "GET /api/dashboard/config HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http default dashboard config", response, 200);
    assert(response.find("{\"widgets\":[]}") != std::string::npos);
    expectStatus("http reject malformed dashboard config", plainRequest(http.boundPort(), post("/api/dashboard/config", "{\"widgets\":[,]}")), 400);
    expectStatus("http save dashboard config", plainRequest(http.boundPort(), post("/api/dashboard/config", "{\"widgets\":[{\"id\":\"one\"}]}")), 200);
    response = plainRequest(http.boundPort(), "GET /api/dashboard/config HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http read saved dashboard config", response, 200);
    assert(response.find("{\"widgets\":[{\"id\":\"one\"}]}") != std::string::npos);
    response = plainRequest(http.boundPort(), "GET /api/logs HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http logs", response, 200);
    assert(response.find("Content-Type: application/json\r\n") != std::string::npos && response.find("\"lines\":") != std::string::npos);
    response = plainRequest(http.boundPort(), "GET /api/devices/0x00124b0000000042/history HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http sensor history", response, 200);
    assert(response.find(",1,2245]") != std::string::npos);
    expectStatus("http reject invalid history cursor", plainRequest(http.boundPort(), "GET /api/devices/0x00124b0000000042/history?before=bad HTTP/1.1\r\nHost: localhost\r\n\r\n"), 400);
    expectStatus("http reject invalid history address", plainRequest(http.boundPort(), "GET /api/devices/wrong/history HTTP/1.1\r\nHost: localhost\r\n\r\n"), 400);
    response = plainRequest(http.boundPort(), "GET /api/scenes/config HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http missing scene config", response, 200);
    assert(response.find("Content-Type: text/plain; charset=utf-8\r\n") != std::string::npos);
    response = plainRequest(http.boundPort(), "GET /api/push/vapid-key HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http VAPID key", response, 200);
    assert(response.find("Content-Type: application/json\r\n") != std::string::npos && response.find("\"public_key\":\"") != std::string::npos);
    expectStatus("http disable notifications", plainRequest(http.boundPort(), post("/api/notifications/enabled", "enabled=false")), 200);
    expectStatus("http reject notification setting", plainRequest(http.boundPort(), post("/api/notifications/enabled", "enabled=maybe")), 400);
    expectStatus("http reject ambiguous notification setting", plainRequest(http.boundPort(), post("/api/notifications/enabled", "enabled=truex")), 400);
    expectStatus("http reject disabled test notification", plainRequest(http.boundPort(), post("/api/notifications/test", "")), 400);
    expectStatus("http enable notifications", plainRequest(http.boundPort(), post("/api/notifications/enabled", "enabled=true")), 200);
    expectStatus("http queue test notification", plainRequest(http.boundPort(), post("/api/notifications/test", "")), 200);
    expectStatus("http reject invalid push subscription", plainRequest(http.boundPort(), post("/api/push/subscriptions", "{}")), 400);
    expectStatus("http save push subscription", plainRequest(http.boundPort(), post("/api/push/subscriptions", subscription("https://push.example.test/subscription"))), 200);
    response = plainRequest(http.boundPort(), "GET /api/push/browser-clients HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http list browser clients", response, 200);
    assert(response.find("Browser") != std::string::npos && response.find("https://push.example.test/subscription") == std::string::npos);
    std::string browserId;
    std::string::size_type browserAt = response.find("browser-");
    assert(browserAt != std::string::npos);
    browserAt += 8;
    browserId = "browser-";
    while (browserAt < response.size() && ((response[browserAt] >= '0' && response[browserAt] <= '9') || (response[browserAt] >= 'a' && response[browserAt] <= 'f')))
        browserId += response[browserAt++];
    expectStatus("http update browser client", plainRequest(http.boundPort(), post(
        "/api/push/browser-clients/" + browserId, "label=Office+browser&enabled=false")), 200);
    response = plainRequest(http.boundPort(), "GET /api/push/browser-clients HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(response.find("Office browser") != std::string::npos && response.find("\"enabled\":false") != std::string::npos);
    expectStatus("http remove browser client", plainRequest(http.boundPort(), post(
        "/api/push/browser-clients/" + browserId + "/delete", "")), 200);
    response = plainRequest(http.boundPort(), "GET /api/push/browser-clients HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(response.find("browser-") == std::string::npos);
    expectStatus("http reject invalid phone", plainRequest(http.boundPort(), post("/api/push/devices/register", "{}")), 400);
    expectStatus("http register phone", plainRequest(http.boundPort(), post("/api/push/devices/register",
        "{\"installation_id\":\"11111111-1111-4111-8111-111111111111\",\"model\":\"Pixel\",\"token\":\"secret-token\"}")), 200);
    response = plainRequest(http.boundPort(), "GET /api/push/devices HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http list phones", response, 200);
    assert(response.find("Pixel") != std::string::npos && response.find("secret-token") == std::string::npos);
    expectStatus("http update phone", plainRequest(http.boundPort(), post(
        "/api/push/devices/11111111-1111-4111-8111-111111111111", "label=Kitchen+phone&enabled=false")), 200);
    response = plainRequest(http.boundPort(), "GET /api/push/devices HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(response.find("Kitchen phone") != std::string::npos && response.find("\"enabled\":false") != std::string::npos);
    expectStatus("http remove phone", plainRequest(http.boundPort(), post(
        "/api/push/devices/11111111-1111-4111-8111-111111111111/delete", "")), 200);
    expectStatus("http reject offline network creation", plainRequest(http.boundPort(), post("/api/network/create", "confirmation=DELETE")), 400);
    expectStatus("http reject invalid scene configuration", plainRequest(http.boundPort(), post("/api/scenes/config", "1 kitchen\n")), 400);
    expectStatus("http reject invalid scene group", plainRequest(http.boundPort(), post("/api/scenes/65528/1/activate", "")), 400);
    expectStatus("http reject scene group zero", plainRequest(http.boundPort(), post("/api/scenes/0/1/activate", "")), 400);
    expectStatus("http reject zero scene", plainRequest(http.boundPort(), post("/api/scenes/1/0/activate", "")), 400);
    expectStatus("http reject oversized scene", plainRequest(http.boundPort(), post("/api/scenes/1/256/activate", "")), 400);
    expectStatus("http reject unknown scene action", plainRequest(http.boundPort(), post("/api/scenes/1/1/delete", "")), 400);
    expectStatus("http report unknown scene group", plainRequest(http.boundPort(), post("/api/scenes/1/off", "")), 400);
    response = plainRequest(http.boundPort(), post("/api/scenes/config",
        "G\t1\tRoom\nM\t1\tlight/test\nS\t1\t1\tReading\nT\t1\t1\tlight/test\t1\t50\n"));
    expectStatus("http save valid scene configuration", response, 200);
    assert(response.find("{\"ok\":true}") != std::string::npos);
    expectStatus("http activate configured scene", plainRequest(http.boundPort(), post("/api/scenes/1/1/activate", "")), 200);
    expectStatus("http turn off configured scene group", plainRequest(http.boundPort(), post("/api/scenes/1/off", "")), 200);
    expectStatus("http reject device settings without name", plainRequest(http.boundPort(), post("/api/devices/light%2Ftest/settings", "low=10")), 400);
    expectStatus("http reject invalid device decimal", plainRequest(http.boundPort(), post("/api/devices/light%2Ftest/settings", "name=Desk&low=ten")), 400);
    expectStatus("http reject invalid device minutes", plainRequest(http.boundPort(), post("/api/devices/light%2Ftest/settings", "name=Desk&initial_minutes=10x&reminder_minutes=5")), 400);
    response = plainRequest(http.boundPort(), post("/api/devices/light%2Ftest/settings", "name=Desk+sensor&low=10.5&high=28&initial_minutes=10&reminder_minutes=5&sensor_refresh_minutes=5"));
    expectStatus("http save encoded device settings", response, 200);
    assert(response.find("{\"ok\":true}") != std::string::npos);
    response = plainRequest(http.boundPort(), post("/api/devices/light%2Ftest/refresh", ""));
    expectStatus("http refresh encoded device", response, 200);
    assert(response.find("Attribute refresh queued") != std::string::npos);
    expectStatus("http report unknown refresh", plainRequest(http.boundPort(), post("/api/devices/unknown/refresh", "")), 400);
    response = plainRequest(http.boundPort(), post("/api/devices/light%2Ftest/identify", ""));
    expectStatus("http identify encoded device", response, 200);
    assert(response.find("Identify command queued") != std::string::npos);
    expectStatus("http reject unknown identify device", plainRequest(http.boundPort(), post("/api/devices/unknown/identify", "")), 400);
    expectStatus("http reject invalid light state", plainRequest(http.boundPort(), post("/api/devices/light%2Ftest/light", "state=blink")), 400);
    response = plainRequest(http.boundPort(), post("/api/devices/light%2Ftest/light", "state=on"));
    expectStatus("http accept encoded light command", response, 200);
    assert(response.find("{\"ok\":true}") != std::string::npos);
    expectStatus("http reject light brightness", plainRequest(http.boundPort(), post("/api/devices/not-a-device/light", "brightness=101")), 400);
    expectStatus("http unknown route", plainRequest(http.boundPort(), "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n"), 404);
    expectStatus("http reject chunked request", plainRequest(http.boundPort(), "POST /api/join HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"), 400);
    expectStatus("http reject conflicting lengths", plainRequest(http.boundPort(), "POST /api/join HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n"), 400);
    expectStatus("http reject duplicate lengths", plainRequest(http.boundPort(), "POST /api/join HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n"), 400);
    expectStatus("http reject malformed request line", plainRequest(http.boundPort(), "INVALID\r\nHost: localhost\r\n\r\n"), 400);
    expectStatus("http reject malformed header", plainRequest(http.boundPort(), "GET /api/status HTTP/1.1\r\nInvalidHeader\r\n\r\n"), 400);
    expectStatus("http reject invalid content length", plainRequest(http.boundPort(), "POST /api/join HTTP/1.1\r\nHost: localhost\r\nContent-Length: nope\r\n\r\n"), 400);
    expectStatus("http reject incomplete request", plainRequestParts(http.boundPort(), std::vector<std::string>(1, "GET /api/status HTTP/1.1\r\nHost: localhost\r\n"), true), 400);
    {
        std::vector<std::string> parts;
        parts.push_back("GET /api/"); parts.push_back("status HTTP/1.1\r\nHost: localhost\r\n\r\n");
        expectStatus("http accept partial request", plainRequestParts(http.boundPort(), parts, false), 200);
    }
    expectStatus("http reject oversized request", plainRequest(http.boundPort(), "POST /api/join HTTP/1.1\r\nHost: localhost\r\nContent-Length: 20000\r\n\r\n"), 413);
    expectStatus("http survives malformed requests", plainRequest(http.boundPort(), "GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n"), 200);

    response = tlsRequest(https.boundPort(), "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("https dashboard", response, 200);
    assert(response.find("Content-Type: text/html; charset=utf-8\r\n") != std::string::npos && response.find("integration dashboard") != std::string::npos);
    response = tlsRequest(https.boundPort(), "GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("https status", response, 200);
    assert(response.find("Content-Type: application/json\r\n") != std::string::npos && response.find("\"devices\":") != std::string::npos);
    response = tlsRequest(https.boundPort(), "GET /api/logs HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("https logs", response, 200);
    assert(response.find("Content-Type: application/json\r\n") != std::string::npos && response.find("\"lines\":") != std::string::npos);
    response = tlsRequest(https.boundPort(), "GET /sw.js HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("https service worker", response, 200);
    assert(response.find("Content-Type: application/javascript; charset=utf-8\r\n") != std::string::npos);
    response = tlsRequest(https.boundPort(), "GET /assets/app.js HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("https javascript asset", response, 200);
    assert(response.find("Content-Type: application/javascript; charset=utf-8\r\n") != std::string::npos);
    response = tlsRequest(https.boundPort(), post("/api/notifications/enabled", "enabled=true"));
    expectStatus("https enable notifications", response, 200);
    assert(response.find("{\"ok\":true,\"enabled\":true}") != std::string::npos);

    {
        HttpServer missingDashboard(0, &runner, directory + "/missing-webui", "", "");
        assert(missingDashboard.start(&error));
        response = plainRequest(missingDashboard.boundPort(), "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        expectStatus("http missing dashboard", response, 500);
        assert(response.find("Dashboard is not installed.") != std::string::npos);
    }

    response = plainRequest(http.boundPort(), post("/api/shelly/config", "id=&name=Test+Uni&host=127.0.0.1&callback_host=127.0.0.1&model=uni&input_index=0&temperature_index=-1&relay_index=0&poll_seconds=30&callback_mode=true"));
    expectStatus("http save probe-free Shelly", response, 200);
    std::string savedId;
    assert(jsonStringField(response.substr(response.find("\r\n\r\n") + 4), "id", &savedId));
    response = plainRequest(http.boundPort(), "GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http unified Shelly status", response, 200);
    assert(response.find("\"source\":\"shelly\"") != std::string::npos);
    assert(response.find("\"external_devices\"") == std::string::npos);
    std::string config = shelly.json(true);
    std::string callbackUrl;
    assert(jsonStringField(config.substr(1, config.size() - 2), "input_open", &callbackUrl));
    std::string target = callbackUrl.substr(callbackUrl.find("/api/"));
    expectStatus("http Shelly callback", plainRequest(http.boundPort(), "GET " + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n"), 200);
    bool contactUpdated = false;
    for (int tries = 0; tries < 100 && !contactUpdated; ++tries) {
        response = plainRequest(http.boundPort(), "GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n");
        std::string::size_type shellyAt = response.find("\"component\":\"contact\",\"external_id\":\"" + savedId + "\"");
        std::string::size_type shellyEnd = shellyAt == std::string::npos ? shellyAt : response.find('}', shellyAt);
        contactUpdated = shellyAt != std::string::npos && shellyEnd != std::string::npos &&
                         response.substr(shellyAt, shellyEnd - shellyAt).find("\"contact_open\":true") != std::string::npos;
        if (!contactUpdated) usleep(20000);
    }
    assert(contactUpdated);
    std::string relayAddress = "0x" + savedId;
    const char* hex = "0123456789abcdef";
    relayAddress[relayAddress.size() - 1] = hex[(static_cast<unsigned int>(std::string(hex).find(relayAddress[relayAddress.size() - 1])) ^ 3) & 15];
    expectStatus("http unified Shelly actor", plainRequest(http.boundPort(), post("/api/devices/" + relayAddress + "/actor", "state=on")), 200);
    response = plainRequest(http.boundPort(), post("/api/shelly/config", "id=&name=Multi+Uni&host=127.0.0.2&callback_host=127.0.0.1&model=uni&input_indices=0,1&temperature_indices=0,1,2&relay_indices=0,1&poll_seconds=30&callback_mode=true"));
    expectStatus("http save multi-channel Shelly", response, 200);
    std::string multiId;
    assert(jsonStringField(response.substr(response.find("\r\n\r\n") + 4), "id", &multiId));
    config = shelly.json(true);
    std::string::size_type multiAt = config.find("\"id\":\"" + multiId + "\"");
    std::string::size_type multiEnd = config.find('}', multiAt);
    assert(multiAt != std::string::npos && multiEnd != std::string::npos);
    std::string multiConfig = "{" + config.substr(multiAt, multiEnd - multiAt + 1);
    std::string secondInputUrl;
    assert(jsonStringField(multiConfig, "input_2_open", &secondInputUrl));
    target = secondInputUrl.substr(secondInputUrl.find("/api/"));
    expectStatus("http Shelly second input callback", plainRequest(http.boundPort(), "GET " + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n"), 200);
    relayAddress = "0x" + multiId;
    relayAddress[relayAddress.size() - 1] = hex[(static_cast<unsigned int>(std::string(hex).find(relayAddress[relayAddress.size() - 1])) ^ 7) & 15];
    expectStatus("http control Shelly second relay", plainRequest(http.boundPort(), post("/api/devices/" + relayAddress + "/actor", "state=on")), 200);
    expectStatus("http remove multi-channel Shelly", plainRequest(http.boundPort(), "DELETE /api/shelly/" + multiId + " HTTP/1.1\r\nHost: localhost\r\n\r\n"), 200);
    expectStatus("http remove Shelly", plainRequest(http.boundPort(), "DELETE /api/shelly/" + savedId + " HTTP/1.1\r\nHost: localhost\r\n\r\n"), 200);
    response = plainRequest(http.boundPort(), "GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(response.find("\"source\":\"shelly\"") == std::string::npos);

    shelly.stop();
    runner.stop();
    notifications.stop();
    close(inbound[1]);
    close(outbound[0]);
    close(outbound[1]);
    removeDirectory(directory);
    signal(SIGPIPE, oldSigpipe);
}

}

void runHttpServerIntegrationTests() {
    testServers();
}
