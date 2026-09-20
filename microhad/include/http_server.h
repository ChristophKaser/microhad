#ifndef MICROHAD_ZIGBEE_HTTP_SERVER_H
#define MICROHAD_ZIGBEE_HTTP_SERVER_H

#include <string>
#include <vector>

class Gateway;
class WebPushStore;

class HttpServer {
public:
    HttpServer(int port, Gateway* gateway, const std::string& certificatePath, const std::string& privateKeyPath);
    ~HttpServer();
    bool start(std::string* error);
    void poll();

private:
    struct Client {
        int fd;
        std::string request;
        std::string response;
        unsigned long responseOffset;
        long deadline;
    };

    int port_;
    int listenFd_;
    Gateway* gateway_;
    WebPushStore* webPush_;
    std::string certificatePath_;
    std::string privateKeyPath_;
    std::vector<Client> clients_;
    void handleClient(int clientFd);
    void handleRequest(int clientFd, const std::string& request);
    void acceptClients();
    void pollClients();
};

#endif
