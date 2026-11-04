extern int microhad_crypto_init(void);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

/* This includes the request line, headers, separator, and body. */
#define MICROHAD_HTTPS_REQUEST_CAPACITY 24576

/* Public IPv4 only. Connect to the resolved address directly so it cannot be
 * resolved a second time after validation. IPv6 is deliberately unsupported. */
int microhad_catalog_public_ipv4(unsigned long address) {
    unsigned long a = address >> 24, b = (address >> 16) & 255, c = (address >> 8) & 255;
    return !(a == 0 || a == 10 || a == 127 || a >= 224 ||
        (a == 100 && b >= 64 && b <= 127) || (a == 169 && b == 254) ||
        (a == 172 && b >= 16 && b <= 31) ||
        (a == 192 && (b == 168 || (b == 0 && (c == 0 || c == 2)) || (b == 88 && c == 99))) ||
        (a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) ||
        (a == 203 && b == 0 && c == 113));
}
static int catalog_connect(mbedtls_net_context* server, const char* host) {
    struct addrinfo hints, *addresses = 0, *at;
    int fd = -1, result = -1;
    memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, "443", &hints, &addresses) != 0 || addresses == 0) return -1;
    for (at = addresses; at; at = at->ai_next) {
        struct sockaddr_in* ip = (struct sockaddr_in*)at->ai_addr;
        if (!microhad_catalog_public_ipv4(ntohl(ip->sin_addr.s_addr))) { freeaddrinfo(addresses); return -1; }
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        struct pollfd pollfd;
        int socket_error = 0; socklen_t length = sizeof(socket_error);
        struct timeval timeout;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
            result = connect(fd, addresses->ai_addr, addresses->ai_addrlen);
            if (result < 0 && errno == EINPROGRESS) {
                pollfd.fd = fd; pollfd.events = POLLOUT; pollfd.revents = 0;
                result = poll(&pollfd, 1, 5000) > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 && socket_error == 0 ? 0 : -1;
            }
            if (result == 0 && fcntl(fd, F_SETFL, flags) != 0) result = -1;
        }
        timeout.tv_sec = 3; timeout.tv_usec = 0;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) result = -1;
    }
    freeaddrinfo(addresses);
    if (result != 0) { if (fd >= 0) close(fd); return -1; }
    server->fd = fd; return 0;
}

static time_t catalog_seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return time(0);
    return now.tv_sec;
}

static int https_request(int catalog_get, const char* host, const char* path, const char* headers, const char* body,
                    unsigned long body_length, const char* ca_bundle, char* response, unsigned long response_capacity,
                    unsigned long* response_length) {
    mbedtls_net_context server;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    char* request = 0;
    int result = -1;
    int header_length;
    const char* stage = "arguments";
    size_t offset = 0;
    unsigned long request_length;

    time_t deadline = catalog_seconds() + 25;
    mbedtls_net_init(&server);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&ca);

    if (host == 0 || path == 0 || headers == 0 || body == 0 || ca_bundle == 0 ||
        response == 0 || response_length == 0 || response_capacity == 0) goto done;
    request = (char*)malloc(MICROHAD_HTTPS_REQUEST_CAPACITY);
    stage = "allocate";
    if (request == 0) goto done;
    stage = "seed";
    result = microhad_crypto_init();
    if (result != 0) goto done;
    stage = "CA bundle";
    result = catalog_get && strncmp(ca_bundle, "-----BEGIN CERTIFICATE-----", 27) == 0 ?
        mbedtls_x509_crt_parse(&ca, (const unsigned char*)ca_bundle, strlen(ca_bundle) + 1) :
        mbedtls_x509_crt_parse_file(&ca, ca_bundle);
    /* A positive result counts skipped certificates in a partly usable bundle. */
    if (result < 0) goto done;
    stage = "connect";
    result = catalog_get ? catalog_connect(&server, host) : mbedtls_net_connect(&server, host, "443", MBEDTLS_NET_PROTO_TCP);
    if (result != 0) goto done;
    stage = "TLS config";
    result = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) goto done;
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &ca, 0);
    stage = "TLS setup";
    result = mbedtls_ssl_setup(&ssl, &conf);
    if (result != 0) goto done;
    stage = "hostname";
    result = mbedtls_ssl_set_hostname(&ssl, host);
    if (result != 0) goto done;
    mbedtls_ssl_set_bio(&ssl, &server, mbedtls_net_send, mbedtls_net_recv, catalog_get ? mbedtls_net_recv_timeout : 0);
    if (catalog_get) mbedtls_ssl_conf_read_timeout(&conf, 3000);
    stage = "handshake";
    while ((result = mbedtls_ssl_handshake(&ssl)) != 0) {
        if ((catalog_get && catalog_seconds() > deadline) || (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE)) goto done;
    }
    stage = "verify certificate";
    if (mbedtls_ssl_get_verify_result(&ssl) != 0) {
        result = -1;
        goto done;
    }
    header_length = catalog_get ? snprintf(request, MICROHAD_HTTPS_REQUEST_CAPACITY,
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept: application/json\r\nAccept-Encoding: identity\r\n\r\n", path, host) :
        snprintf(request, MICROHAD_HTTPS_REQUEST_CAPACITY,
                             "POST %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Length: %lu\r\n%s\r\n",
                             path, host, body_length, headers);
    if (header_length < 0 || header_length >= MICROHAD_HTTPS_REQUEST_CAPACITY ||
        body_length > MICROHAD_HTTPS_REQUEST_CAPACITY - (unsigned long)header_length) {
        result = -1;
        goto done;
    }
    memcpy(request + header_length, body, body_length);
    request_length = (unsigned long)header_length + body_length;
    stage = "write";
    while (offset < request_length) {
        if (catalog_get && catalog_seconds() > deadline) { result = -1; goto done; }
        result = mbedtls_ssl_write(&ssl, (const unsigned char*)request + offset, request_length - offset);
        if (result > 0) offset += (size_t)result;
        else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) goto done;
    }
    *response_length = 0;
    stage = "read";
    for (;;) {
        if (catalog_get && catalog_seconds() > deadline) { result = -1; goto done; }
        if (*response_length == response_capacity) {
            result = -1;
            goto done;
        }
        result = mbedtls_ssl_read(&ssl, (unsigned char*)response + *response_length,
                                  response_capacity - *response_length);
        if (result > 0) *response_length += (unsigned long)result;
        else if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            result = 0;
            break;
        } else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) goto done;
    }
done:
    if (result != 0) {
        char description[128];
        mbedtls_strerror(result, description, sizeof(description));
        fprintf(stderr, "HTTPS %s failed for %s: code=%d (%s), verify=0x%lx\n",
                stage, host == 0 ? "unknown" : host, result, description,
                (unsigned long)mbedtls_ssl_get_verify_result(&ssl));
    }
    free(request);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_net_free(&server);
    mbedtls_x509_crt_free(&ca);

    return result;
}

int microhad_https_post(const char* host, const char* path, const char* headers, const char* body,
    unsigned long body_length, const char* ca_bundle, char* response, unsigned long capacity, unsigned long* length) {
    return https_request(0, host, path, headers, body, body_length, ca_bundle, response, capacity, length);
}
int microhad_https_catalog_get(const char* host, const char* path, const char* ca_bundle,
    char* response, unsigned long capacity, unsigned long* length) {
    return https_request(1, host, path, "", "", 0, ca_bundle, response, capacity, length);
}

typedef struct microhad_https_session {
    char host[256];
    mbedtls_net_context server;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
} microhad_https_session;

void microhad_https_session_close(void* opaque) {
    microhad_https_session* s = (microhad_https_session*)opaque;
    if (s == 0) return;
    mbedtls_ssl_free(&s->ssl); mbedtls_ssl_config_free(&s->conf);
    mbedtls_net_free(&s->server); mbedtls_x509_crt_free(&s->ca);
    free(s);
}

void* microhad_https_session_open(const char* host, const char* ca_bundle) {
    microhad_https_session* s = (microhad_https_session*)calloc(1, sizeof(microhad_https_session));
    int result;
    if (s == 0) return 0;
    mbedtls_net_init(&s->server); mbedtls_ssl_init(&s->ssl); mbedtls_ssl_config_init(&s->conf);
    mbedtls_x509_crt_init(&s->ca);
    result = microhad_crypto_init();
    if (result == 0) result = mbedtls_x509_crt_parse_file(&s->ca, ca_bundle);
    if (result >= 0) result = mbedtls_net_connect(&s->server, host, "443", MBEDTLS_NET_PROTO_TCP);
    if (result == 0) result = mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_CLIENT,
                              MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result == 0) {
        mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&s->conf, &s->ca, 0);
        result = mbedtls_ssl_setup(&s->ssl, &s->conf);
    }
    if (result == 0) result = mbedtls_ssl_set_hostname(&s->ssl, host);
    if (result == 0) mbedtls_ssl_set_bio(&s->ssl, &s->server, mbedtls_net_send, mbedtls_net_recv, 0);
    while (result == 0 && (result = mbedtls_ssl_handshake(&s->ssl)) != 0) {
        if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) break;
    }
    if (result == 0 && mbedtls_ssl_get_verify_result(&s->ssl) != 0) result = -1;
    if (result != 0) { microhad_https_session_close(s); return 0; }
    strncpy(s->host, host, sizeof(s->host) - 1);
    return s;
}

/* FCM replies carry Content-Length. Read exactly one response so the TLS stream
 * remains positioned for the next request. */
int microhad_https_session_post(void* opaque, const char* path, const char* headers, const char* body,
                            unsigned long body_length, char* response, unsigned long response_capacity,
                            unsigned long* response_length) {
    microhad_https_session* s = (microhad_https_session*)opaque;
    char* request;
    int header_length, result = -1;
    unsigned long request_length, offset = 0, body_at, expected = 0;
    int has_length = 0, close_after = 0;
    if (s == 0 || path == 0 || headers == 0 || body == 0 || response == 0 || response_length == 0) return -1;
    request = (char*)malloc(MICROHAD_HTTPS_REQUEST_CAPACITY);
    if (request == 0) return -1;
    header_length = snprintf(request, MICROHAD_HTTPS_REQUEST_CAPACITY,
                             "POST %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\nContent-Length: %lu\r\n%s\r\n",
                             path, s->host, body_length, headers);
    if (header_length < 0 || header_length >= MICROHAD_HTTPS_REQUEST_CAPACITY ||
        body_length > MICROHAD_HTTPS_REQUEST_CAPACITY - (unsigned long)header_length) goto done_session;
    memcpy(request + header_length, body, body_length);
    request_length = (unsigned long)header_length + body_length;
    while (offset < request_length) {
        result = mbedtls_ssl_write(&s->ssl, (const unsigned char*)request + offset, request_length - offset);
        if (result > 0) offset += (unsigned long)result;
        else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) goto done_session;
    }
    *response_length = 0;
    /* Read response headers one byte at a time to avoid consuming bytes from a
     * later response and to keep the parser small on the MIPS target. */
    while (*response_length + 1 < response_capacity) {
        result = mbedtls_ssl_read(&s->ssl, (unsigned char*)response + *response_length, 1);
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (result <= 0) goto done_session;
        ++*response_length;
        if (*response_length >= 4 && memcmp(response + *response_length - 4, "\r\n\r\n", 4) == 0) break;
    }
    if (*response_length < 4 || memcmp(response + *response_length - 4, "\r\n\r\n", 4) != 0) goto done_session;
    body_at = *response_length;
    {
        unsigned long i;
        for (i = 0; i + 17 < body_at; ++i) {
            if ((i == 0 || response[i-1] == '\n') && strncasecmp(response + i, "Connection:", 11) == 0) {
                unsigned long at = i + 11;
                while (at < body_at && (response[at] == ' ' || response[at] == '\t')) ++at;
                if (strncasecmp(response + at, "close", 5) == 0) close_after = 1;
            }
        }
        for (i = 0; i + 15 < body_at; ++i) {
            if ((i == 0 || response[i-1] == '\n') && strncasecmp(response + i, "Content-Length:", 15) == 0) {
                unsigned long at = i + 15;
                while (at < body_at && (response[at] == ' ' || response[at] == '\t')) ++at;
                while (at < body_at && response[at] >= '0' && response[at] <= '9') {
                    expected = expected * 10 + (unsigned long)(response[at++] - '0'); has_length = 1;
                }
                break;
            }
        }
    }
    if (!has_length || expected > response_capacity - *response_length) goto done_session;
    while (*response_length - body_at < expected) {
        result = mbedtls_ssl_read(&s->ssl, (unsigned char*)response + *response_length,
                                  expected - (*response_length - body_at));
        if (result > 0) *response_length += (unsigned long)result;
        else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) goto done_session;
    }
    result = close_after ? 1 : 0;
done_session:
    free(request);
    return result;
}
