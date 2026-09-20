#ifndef MICROHAD_ZIGBEE_HTTPS_CLIENT_H
#define MICROHAD_ZIGBEE_HTTPS_CLIENT_H

#include <string>

bool httpsPost(const std::string& host, const std::string& path, const std::string& headers,
               const std::string& body, const std::string& caBundle, std::string* response, std::string* error);

#endif
