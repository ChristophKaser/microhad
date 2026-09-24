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

/* A deterministic host-test transport override; production leaves it null. */
void setHttpsPostHook(HttpsPostHook* hook);

bool httpsPost(const std::string& host, const std::string& path, const std::string& headers,
               const std::string& body, const std::string& caBundle, std::string* response, std::string* error);
bool httpsPostUrl(const std::string& url, const std::string& headers, const std::string& body,
                  const std::string& caBundle, std::string* response, std::string* error);

#endif
