extern int microhad_crypto_init(void);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

/* This includes the request line, headers, separator, and body. */
#define MICROHAD_HTTPS_REQUEST_CAPACITY 24576

int microhad_https_post(const char* host, const char* path, const char* headers, const char* body,
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
    result = mbedtls_x509_crt_parse_file(&ca, ca_bundle);
    /* A positive result counts skipped certificates in a partly usable bundle. */
    if (result < 0) goto done;
    stage = "connect";
    result = mbedtls_net_connect(&server, host, "443", MBEDTLS_NET_PROTO_TCP);
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
    mbedtls_ssl_set_bio(&ssl, &server, mbedtls_net_send, mbedtls_net_recv, 0);
    stage = "handshake";
    while ((result = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) goto done;
    }
    stage = "verify certificate";
    if (mbedtls_ssl_get_verify_result(&ssl) != 0) {
        result = -1;
        goto done;
    }
    header_length = snprintf(request, MICROHAD_HTTPS_REQUEST_CAPACITY,
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
        result = mbedtls_ssl_write(&ssl, (const unsigned char*)request + offset, request_length - offset);
        if (result > 0) offset += (size_t)result;
        else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) goto done;
    }
    *response_length = 0;
    stage = "read";
    for (;;) {
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
