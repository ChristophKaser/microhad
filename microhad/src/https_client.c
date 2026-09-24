#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
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
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    char* request = 0;
    int result = -1;
    int header_length;
    size_t offset = 0;
    unsigned long request_length;

    mbedtls_net_init(&server);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&ca);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_init(&entropy);
    if (host == 0 || path == 0 || headers == 0 || body == 0 || ca_bundle == 0 ||
        response == 0 || response_length == 0 || response_capacity == 0) goto done;
    request = (char*)malloc(MICROHAD_HTTPS_REQUEST_CAPACITY);
    if (request == 0) goto done;
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char*)"microhad-https", 10) != 0) goto done;
    if (mbedtls_x509_crt_parse_file(&ca, ca_bundle) != 0 ||
        mbedtls_net_connect(&server, host, "443", MBEDTLS_NET_PROTO_TCP) != 0) goto done;
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto done;
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &ca, 0);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    if (mbedtls_ssl_setup(&ssl, &conf) != 0 || mbedtls_ssl_set_hostname(&ssl, host) != 0) goto done;
    mbedtls_ssl_set_bio(&ssl, &server, mbedtls_net_send, mbedtls_net_recv, 0);
    while ((result = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) goto done;
    }
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
    while (offset < request_length) {
        result = mbedtls_ssl_write(&ssl, (const unsigned char*)request + offset, request_length - offset);
        if (result > 0) offset += (size_t)result;
        else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) goto done;
    }
    *response_length = 0;
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
    free(request);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_net_free(&server);
    mbedtls_x509_crt_free(&ca);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return result;
}
