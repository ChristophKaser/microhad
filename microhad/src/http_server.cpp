#include "http_server.h"
#include "gateway.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace {
void writeResponse(int fd, int status, const char* contentType, const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << (status == 200 ? " OK" : " Bad Request") << "\r\n"
             << "Content-Type: " << contentType << "\r\nCache-Control: no-store\r\nConnection: close\r\n"
             << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::string wire = response.str();
    const char* cursor = wire.data();
    unsigned long remaining = wire.size();
    while (remaining != 0) {
        ssize_t written = write(fd, cursor, remaining);
        if (written <= 0) break;
        cursor += written;
        remaining -= static_cast<unsigned long>(written);
    }
}
}

HttpServer::HttpServer(int port, Gateway* gateway) : port_(port), listenFd_(-1), gateway_(gateway) {
}

HttpServer::~HttpServer() {
    if (listenFd_ >= 0) close(listenFd_);
}

bool HttpServer::start(std::string* error) {
    struct sockaddr_in address;
    int reuse = 1;
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) { *error = strerror(errno); return false; }
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<unsigned short>(port_));
    if (bind(listenFd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0 || listen(listenFd_, 8) != 0) {
        *error = strerror(errno); close(listenFd_); listenFd_ = -1; return false;
    }
    fcntl(listenFd_, F_SETFL, O_NONBLOCK);
    return true;
}

void HttpServer::poll() {
    int clientFd = accept(listenFd_, 0, 0);
    if (clientFd >= 0) {
        handleClient(clientFd);
        close(clientFd);
    }
}

void HttpServer::handleClient(int clientFd) {
    char buffer[2048];
    ssize_t count = read(clientFd, buffer, sizeof(buffer) - 1);
    std::string request;
    if (count <= 0) return;
    buffer[count] = 0;
    request.assign(buffer);
    if (request.find("GET /api/status ") == 0) {
        writeResponse(clientFd, 200, "application/json", gateway_->statusJson());
    } else if (request.find("POST /api/join ") == 0) {
        std::string error;
        if (gateway_->setJoining(300, &error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"seconds\":300}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + error + "\"}");
    } else if (request.find("GET / ") == 0) {
        const char* page = "<!doctype html><title>MicroHAD</title><h1>MicroHAD Native</h1><button onclick=\"fetch('/api/join',{method:'POST'})\">Join for 5 minutes</button><pre id=s></pre><script>setInterval(function(){fetch('/api/status').then(function(r){return r.json()}).then(function(v){document.getElementById('s').textContent=JSON.stringify(v,null,2)})},1000)</script>";
        writeResponse(clientFd, 200, "text/html; charset=utf-8", page);
    } else {
        writeResponse(clientFd, 400, "application/json", "{\"error\":\"unknown endpoint\"}");
    }
}
