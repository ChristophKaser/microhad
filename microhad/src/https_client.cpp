#include "https_client.h"

#ifdef MICROHAD_MIPS_TARGET
extern "C" int microhad_https_post(const char* host, const char* path, const char* headers, const char* body,
                                const char* caBundle, char* response, unsigned long responseCapacity,
                                unsigned long* responseLength);
#endif

bool httpsPost(const std::string& host, const std::string& path, const std::string& headers,
               const std::string& body, const std::string& caBundle, std::string* response, std::string* error) {
#ifdef MICROHAD_MIPS_TARGET
    std::string buffer(16384, '\0');
    unsigned long length = 0;
    if (caBundle.empty()) { *error = "a CA bundle is required for HTTPS"; return false; }
    if (microhad_https_post(host.c_str(), path.c_str(), headers.c_str(), body.c_str(), caBundle.c_str(), &buffer[0],
                        buffer.size(), &length) != 0) { *error = "HTTPS request failed"; return false; }
    response->assign(buffer.data(), length);
    return true;
#else
    (void)host; (void)path; (void)headers; (void)body; (void)caBundle; (void)response;
    *error = "HTTPS transport is available only in the RSDK MIPS build";
    return false;
#endif
}

bool httpsPostUrl(const std::string& url, const std::string& headers, const std::string& body,
                  const std::string& caBundle, std::string* response, std::string* error) {
    const std::string prefix = "https://";
    std::string::size_type start;
    std::string::size_type slash;
    if (url.compare(0, prefix.size(), prefix) != 0) { *error = "Push endpoint must use HTTPS"; return false; }
    start = prefix.size();
    slash = url.find('/', start);
    if (slash == std::string::npos) return httpsPost(url.substr(start), "/", headers, body, caBundle, response, error);
    return httpsPost(url.substr(start, slash - start), url.substr(slash), headers, body, caBundle, response, error);
}
