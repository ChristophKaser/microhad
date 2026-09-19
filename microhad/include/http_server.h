#ifndef MICROHAD_ZIGBEE_HTTP_SERVER_H
#define MICROHAD_ZIGBEE_HTTP_SERVER_H

#include <string>

class Gateway;

class HttpServer {
public:
    HttpServer(int port, Gateway* gateway);
    ~HttpServer();
    bool start(std::string* error);
    void poll();

private:
    int port_;
    int listenFd_;
    Gateway* gateway_;
    void handleClient(int clientFd);
};

#endif
