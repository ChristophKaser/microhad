#include "https_client.h"

#include <pthread.h>

namespace {
pthread_mutex_t httpsHookMutex = PTHREAD_MUTEX_INITIALIZER;
HttpsPostHook* httpsPostHook = 0;
}

void setHttpsPostHook(HttpsPostHook* hook) {
    pthread_mutex_lock(&httpsHookMutex);
    httpsPostHook = hook;
    pthread_mutex_unlock(&httpsHookMutex);
}

#ifdef MICROHAD_MBEDTLS_ENABLED
extern "C" int microhad_https_post(const char* host, const char* path, const char* headers, const char* body,
                                unsigned long bodyLength, const char* caBundle, char* response, unsigned long responseCapacity,
                                unsigned long* responseLength);
#endif

bool httpsPost(const std::string& host, const std::string& path, const std::string& headers,
               const std::string& body, const std::string& caBundle, std::string* response, std::string* error) {
#ifdef MICROHAD_MBEDTLS_ENABLED
    std::string buffer(16384, '\0');
    unsigned long length = 0;
    if (caBundle.empty()) { *error = "a CA bundle is required for HTTPS"; return false; }
    pthread_mutex_lock(&httpsHookMutex);
    HttpsPostHook* hook = httpsPostHook;
    pthread_mutex_unlock(&httpsHookMutex);
    if (hook != 0) return hook->post(host, path, headers, body, caBundle, response, error);
    if (microhad_https_post(host.c_str(), path.c_str(), headers.c_str(), body.data(), body.size(), caBundle.c_str(), &buffer[0],
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
    std::string::size_type pathAt;
    std::string host;
    if (url.compare(0, prefix.size(), prefix) != 0) { *error = "Push endpoint must use HTTPS"; return false; }
    start = prefix.size();
    pathAt = url.find_first_of("/?#", start);
    host = url.substr(start, pathAt == std::string::npos ? std::string::npos : pathAt - start);
    if (host.empty() || host.find('@') != std::string::npos || url.find_first_of(" \t\r\n") != std::string::npos ||
        (pathAt != std::string::npos && url[pathAt] == '#') || url.find('#', start) != std::string::npos) {
        *error = "invalid HTTPS URL";
        return false;
    }
    if (pathAt == std::string::npos) return httpsPost(host, "/", headers, body, caBundle, response, error);
    if (url[pathAt] == '?') return httpsPost(host, "/" + url.substr(pathAt), headers, body, caBundle, response, error);
    return httpsPost(host, url.substr(pathAt), headers, body, caBundle, response, error);
}
