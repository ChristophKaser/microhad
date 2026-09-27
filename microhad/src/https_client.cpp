#include "https_client.h"

#include <cctype>
#include <climits>
#include <pthread.h>

namespace {
pthread_mutex_t httpsHookMutex = PTHREAD_MUTEX_INITIALIZER;
HttpsPostHook* httpsPostHook = 0;

bool decodeChunkedResponse(std::string* response, std::string* error) {
    std::string::size_type bodyAt = response->find("\r\n\r\n");
    if (bodyAt == std::string::npos) return true;
    std::string headers = response->substr(0, bodyAt);
    for (std::string::size_type i = 0; i < headers.size(); ++i)
        headers[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(headers[i])));
    std::string::size_type transferAt = headers.find("\r\ntransfer-encoding:");
    if (transferAt == std::string::npos) return true;
    std::string::size_type valueEnd = headers.find("\r\n", transferAt + 2);
    std::string transfer = headers.substr(transferAt, valueEnd == std::string::npos ?
                                   std::string::npos : valueEnd - transferAt);
    if (transfer.find("chunked") == std::string::npos) return true;

    std::string decoded;
    std::string::size_type at = bodyAt + 4;
    for (;;) {
        std::string::size_type lineEnd = response->find("\r\n", at);
        if (lineEnd == std::string::npos) break;
        unsigned long chunkSize = 0;
        std::string::size_type i = at;
        for (; i < lineEnd && (*response)[i] != ';'; ++i) {
            unsigned char c = static_cast<unsigned char>((*response)[i]);
            unsigned long digit = c >= '0' && c <= '9' ? c - '0' :
                                  c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                                  c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (digit == 16 || chunkSize > (ULONG_MAX - digit) / 16) break;
            chunkSize = chunkSize * 16 + digit;
        }
        if (i == at || (i < lineEnd && (*response)[i] != ';')) break;
        at = lineEnd + 2;
        if (chunkSize == 0) {
            response->replace(bodyAt + 4, response->size() - bodyAt - 4, decoded);
            return true;
        }
        if (chunkSize > response->size() - at || response->size() - at - chunkSize < 2 ||
            response->compare(at + chunkSize, 2, "\r\n") != 0) break;
        decoded.append(*response, at, chunkSize);
        at += chunkSize + 2;
    }
    *error = "HTTPS response has invalid chunked encoding";
    return false;
}
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
    return decodeChunkedResponse(response, error);
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
