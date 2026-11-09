#ifndef MICROHAD_ZIGBEE_HTTPS_CLIENT_H
#define MICROHAD_ZIGBEE_HTTPS_CLIENT_H

#include <string>

class HttpsPostHook {
public:
    virtual ~HttpsPostHook() {}
    virtual bool post(const std::string& host, const std::string& path, const std::string& headers,
                      const std::string& body, const std::string& caBundle,
                      std::string* response, std::string* error) = 0;
};

class HttpsSessionHook {
public:
    virtual ~HttpsSessionHook() {}
    virtual bool open(const std::string& host, const std::string& caBundle, std::string* error) = 0;
    virtual bool post(const std::string& host, const std::string& path, const std::string& headers,
                      const std::string& body, const std::string& caBundle,
                      std::string* response, std::string* error) = 0;
    virtual void close() = 0;
};

/* A deterministic host-test transport override; production leaves it null. */
void setHttpsPostHook(HttpsPostHook* hook);
void setHttpsSessionHook(HttpsSessionHook* hook);

class HttpsSession {
public:
    HttpsSession();
    ~HttpsSession();
    bool post(const std::string& host, const std::string& path, const std::string& headers,
              const std::string& body, const std::string& caBundle,
              std::string* response, std::string* error);
    void close();
private:
    void* session_;
    std::string host_, caBundle_;
    HttpsSessionHook* hook_;
    bool hookConnected_;
};

bool httpsPost(const std::string& host, const std::string& path, const std::string& headers,
               const std::string& body, const std::string& caBundle, std::string* response, std::string* error);
bool httpsPostUrl(const std::string& url, const std::string& headers, const std::string& body,
                  const std::string& caBundle, std::string* response, std::string* error);

class HttpsCatalogRawSink {
public:
    virtual ~HttpsCatalogRawSink() {}
    virtual bool write(const char* data, unsigned long size) = 0;
};
class HttpsCatalogHook {
public:
    virtual ~HttpsCatalogHook() {}
    virtual bool stream(const std::string& host, const std::string& path, const std::string& ca,
                        HttpsCatalogRawSink* sink, std::string* error) = 0;
};
void setHttpsCatalogHook(HttpsCatalogHook* hook);
bool httpsCatalogStream(const std::string& host, const std::string& path, const std::string& caBundle, HttpsCatalogRawSink* sink, std::string* error);
#endif
