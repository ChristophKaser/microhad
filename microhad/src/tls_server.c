#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

struct microhad_tls_session {
    mbedtls_net_context socket;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context private_key;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
};

static char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

static char* content_length_header(char* request) {
    const char* name = "content-length:";
    char* line = request;
    while (*line != 0) {
        unsigned long i = 0;
        while (name[i] != 0 && line[i] != 0 && ascii_lower(line[i]) == name[i]) i++;
        if (name[i] == 0) return line + i;
        line = strstr(line, "\r\n");
        if (line == 0) break;
        line += 2;
    }
    return 0;
}

static void release_session(struct microhad_tls_session* session) {
    if (session == 0) return;
    mbedtls_ssl_free(&session->ssl); mbedtls_ssl_config_free(&session->config);
    /* HttpServer owns the descriptor and closes it after this TLS session. */
    session->socket.fd = -1;
    mbedtls_net_free(&session->socket); mbedtls_x509_crt_free(&session->certificate);
    mbedtls_pk_free(&session->private_key); mbedtls_ctr_drbg_free(&session->drbg);
    mbedtls_entropy_free(&session->entropy); free(session);
}

int microhad_tls_read_request(int fd, const char* certificate_path, const char* private_key_path,
                          char* request, unsigned long capacity, void** output_session) {
    struct microhad_tls_session* session;
    int result = -1;
    int received;
    unsigned long length = 0;
    unsigned long expected = 0;
    if (capacity < 2 || output_session == 0) return -1;
    *output_session = 0;
    session = (struct microhad_tls_session*)calloc(1, sizeof(*session));
    if (session == 0) return -1;
    mbedtls_net_init(&session->socket); mbedtls_ssl_init(&session->ssl); mbedtls_ssl_config_init(&session->config);
    mbedtls_x509_crt_init(&session->certificate); mbedtls_pk_init(&session->private_key);
    mbedtls_ctr_drbg_init(&session->drbg); mbedtls_entropy_init(&session->entropy);
    session->socket.fd = fd;
    result = mbedtls_ctr_drbg_seed(&session->drbg, mbedtls_entropy_func, &session->entropy,
                                   (const unsigned char*)"microhad-web-server", 15);
    if (result == 0) result = mbedtls_x509_crt_parse_file(&session->certificate, certificate_path);
    if (result == 0) result = mbedtls_pk_parse_keyfile(&session->private_key, private_key_path, 0);
    if (result == 0) result = mbedtls_ssl_config_defaults(&session->config, MBEDTLS_SSL_IS_SERVER,
                                                           MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result == 0) mbedtls_ssl_conf_rng(&session->config, mbedtls_ctr_drbg_random, &session->drbg);
    if (result == 0) result = mbedtls_ssl_conf_own_cert(&session->config, &session->certificate, &session->private_key);
    if (result == 0) result = mbedtls_ssl_setup(&session->ssl, &session->config);
    if (result == 0) mbedtls_ssl_set_bio(&session->ssl, &session->socket, mbedtls_net_send, mbedtls_net_recv, 0);
    while (result == 0 && (received = mbedtls_ssl_handshake(&session->ssl)) != 0) {
        if (received != MBEDTLS_ERR_SSL_WANT_READ && received != MBEDTLS_ERR_SSL_WANT_WRITE) result = received;
    }
    while (result == 0 && length < capacity - 1) {
        received = mbedtls_ssl_read(&session->ssl, (unsigned char*)request + length, capacity - 1 - length);
        if (received > 0) {
            length += (unsigned long)received;
            request[length] = 0;
            if (expected == 0) {
                char* header_end = strstr(request, "\r\n\r\n");
                if (header_end != 0) {
                    char* content_length = content_length_header(request);
                    unsigned long header_length = (unsigned long)(header_end + 4 - request);
                    expected = header_length;
                    if (content_length != 0) expected += strtoul(content_length, 0, 10);
                }
            }
            if (expected != 0 && length >= expected) break;
        }
        else if (received != MBEDTLS_ERR_SSL_WANT_READ && received != MBEDTLS_ERR_SSL_WANT_WRITE) { result = -1; }
    }
    if (result == 0) { request[length] = 0; *output_session = session; return 0; }
    release_session(session);
    return -1;
}

int microhad_tls_write_close(void* opaque_session, const char* response, unsigned long length) {
    struct microhad_tls_session* session = (struct microhad_tls_session*)opaque_session;
    unsigned long offset = 0;
    int result = 0;
    while (offset < length) {
        result = mbedtls_ssl_write(&session->ssl, (const unsigned char*)response + offset, length - offset);
        if (result > 0) offset += (unsigned long)result;
        else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) break;
    }
    mbedtls_ssl_close_notify(&session->ssl);
    release_session(session);
    return result < 0 ? result : 0;
}
