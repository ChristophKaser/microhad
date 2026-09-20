#include "https_client.h"

#ifdef MICROHAD_MIPS_TARGET
extern "C" int microhad_https_post(const char* host, const char* path, const char* headers, const char* body,
                                const char* caBundle, char* response, unsigned long responseCapacity,
                                unsigned long* responseLength);
#endif

bool httpsPost(const std::string& host, const std::string& path, const std::string& headers,
               const std::string& body, const std::string& caBundle, std::string* response, std::string* error) {
#ifdef MICROHAD_MIPS_TARGET
    char buffer[16384];
    unsigned long length = 0;
    if (caBundle.empty()) { *error = "a CA bundle is required for HTTPS"; return false; }
    if (microhad_https_post(host.c_str(), path.c_str(), headers.c_str(), body.c_str(), caBundle.c_str(), buffer,
                        sizeof(buffer), &length) != 0) { *error = "HTTPS request failed"; return false; }
    response->assign(buffer, length);
    return true;
#else
    (void)host; (void)path; (void)headers; (void)body; (void)caBundle; (void)response;
    *error = "HTTPS transport is available only in the RSDK MIPS build";
    return false;
#endif
}
