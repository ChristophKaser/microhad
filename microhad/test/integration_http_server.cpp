#include "gateway.h"
#include "gateway_runner.h"
#include "gateway_integration_peer.h"
#include "http_server.h"
#include "notification_worker.h"
#include "fcm.h"
#include "storage.h"

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
    rmdir((directory + "/webui").c_str());
    unlink((directory + "/notifications.json").c_str());
    unlink((directory + "/vapid-public.key").c_str());
    unlink((directory + "/vapid-private.key").c_str());
    unlink((directory + "/web-push-subscriptions.ndjson").c_str());
    unlink((directory + "/scenes.conf").c_str());
    unlink((directory + "/devices.json").c_str());
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
    std::string directory = makeDirectory();
    std::string error;
    int inbound[2], outbound[2];
    assert(pipe(inbound) == 0 && pipe(outbound) == 0);
    assert(ensureDirectory(directory + "/webui", &error));
    writeText(directory + "/webui/index.html", "<html>integration dashboard</html>");
    NotificationWorker notifications(directory, "", "", "");
    assert(notifications.start(&error));
    Gateway* gateway = new Gateway("/dev/null", 115200, directory, "", "", "", &notifications);
    GatewayIntegrationTestPeer::attachSerial(gateway, inbound[0]);
    GatewayRunner runner(gateway);
    assert(runner.start(&error));
    GatewayIntegrationTestPeer::prepareHttpDevice(gateway);

    HttpServer http(0, &runner, directory + "/webui/index.html", "", "");
    HttpServer https(0, &runner, directory + "/webui/index.html", "test/fixtures/tls/localhost.crt", "test/fixtures/tls/localhost.key");
    if (!http.start(&error)) { fprintf(stderr, "HTTP start failed: %s\n", error.c_str()); assert(false); }
    if (!https.start(&error)) { fprintf(stderr, "HTTPS start failed: %s\n", error.c_str()); assert(false); }
    assert(http.boundPort() > 0 && https.boundPort() > 0);

    std::string response = plainRequest(http.boundPort(), "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http dashboard", response, 200);
    assert(response.find("Content-Type: text/html; charset=utf-8\r\n") != std::string::npos && response.find("<html>integration dashboard</html>") != std::string::npos);
    response = plainRequest(http.boundPort(), "GET /sw.js HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http service worker", response, 200);
    assert(response.find("Content-Type: application/javascript\r\n") != std::string::npos && response.find("addEventListener('push'") != std::string::npos);
    response = plainRequest(http.boundPort(), "GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http status", response, 200);
    assert(response.find("Content-Type: application/json\r\n") != std::string::npos && response.find("\"online\":") != std::string::npos && response.find("\"devices\":") != std::string::npos);
    response = plainRequest(http.boundPort(), "GET /api/logs HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatus("http logs", response, 200);
    assert(response.find("Content-Type: application/json\r\n") != std::string::npos && response.find("\"lines\":") != std::string::npos);
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
    expectStatus("http reject invalid light state", plainRequest(http.boundPort(), post("/api/devices/light%2Ftest/light", "state=blink")), 400);
    response = plainRequest(http.boundPort(), post("/api/devices/light%2Ftest/light", "state=on"));
    expectStatus("http accept encoded light command", response, 200);
    assert(response.find("{\"ok\":true}") != std::string::npos);
    expectStatus("http reject light brightness", plainRequest(http.boundPort(), post("/api/devices/not-a-device/light", "brightness=101")), 400);
    expectStatus("http unknown route", plainRequest(http.boundPort(), "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n"), 400);
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
    assert(response.find("Content-Type: application/javascript\r\n") != std::string::npos);
    response = tlsRequest(https.boundPort(), post("/api/notifications/enabled", "enabled=true"));
    expectStatus("https enable notifications", response, 200);
    assert(response.find("{\"ok\":true,\"enabled\":true}") != std::string::npos);

    {
        HttpServer missingDashboard(0, &runner, directory + "/missing-dashboard.html", "", "");
        assert(missingDashboard.start(&error));
        response = plainRequest(missingDashboard.boundPort(), "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        expectStatus("http missing dashboard", response, 500);
        assert(response.find("Dashboard is not installed.") != std::string::npos);
    }

    runner.stop();
    notifications.stop();
    close(inbound[1]);
    close(outbound[0]);
    close(outbound[1]);
    removeDirectory(directory);
}

}

void runHttpServerIntegrationTests() {
    testServers();
}
