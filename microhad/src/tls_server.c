extern int microhad_crypto_init(void);
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cache.h>
#include <mbedtls/x509_crt.h>

struct microhad_tls_server {
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context private_key;
    mbedtls_ssl_cache_context cache;
    int* ciphersuites;
};

struct microhad_tls_session {
    mbedtls_net_context socket;
    mbedtls_ssl_context ssl;
    struct microhad_tls_server* server;
};

/* P-521 ECDHE costs roughly two seconds on the gateway's MIPS CPU. P-256 is
 * widely supported by browsers and is also the curve used for Web Push. */
static const uint16_t server_curves[] = {
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
    0
};

/* ChaCha20 costs substantially less CPU than AES-GCM on the gateway.
 * Reorder the library list rather than removing compatible fallback suites. */
static int* server_ciphersuites(void) {
    const int* defaults = mbedtls_ssl_list_ciphersuites();
    int count = 0, offset = 0, pass, i;
    int* preferred;
    while (defaults[count] != 0) ++count;
    preferred = (int*)calloc(count + 1, sizeof(*preferred));
    if (preferred == NULL) return NULL;
    for (pass = 0; pass < 2; ++pass) {
        for (i = 0; i < count; ++i) {
            int chacha = defaults[i] == MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256 ||
                         defaults[i] == MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256;
            if (chacha == (pass == 0)) preferred[offset++] = defaults[i];
        }
    }
    return preferred;
}

static void release_session(struct microhad_tls_session* session) {
    if (session == 0) return;
    mbedtls_ssl_free(&session->ssl);
    /* HttpServer owns the descriptor and closes it after this TLS session. */
    session->socket.fd = -1;
    mbedtls_net_free(&session->socket); free(session);
}

int microhad_tls_server_create(const char* certificate_path, const char* private_key_path, void** output_server) {
    struct microhad_tls_server* server;
    int result;
    if (output_server == 0) return -1;
    *output_server = 0;
    server = (struct microhad_tls_server*)calloc(1, sizeof(*server));
    if (server == 0) return -1;
    mbedtls_ssl_config_init(&server->config); mbedtls_x509_crt_init(&server->certificate);
    mbedtls_pk_init(&server->private_key);
    mbedtls_ssl_cache_init(&server->cache);
    result = microhad_crypto_init();
    if (result == 0) result = mbedtls_x509_crt_parse_file(&server->certificate, certificate_path);
    if (result == 0) result = mbedtls_pk_parse_keyfile(&server->private_key, private_key_path, 0);
    if (result == 0) result = mbedtls_ssl_config_defaults(&server->config, MBEDTLS_SSL_IS_SERVER,
                                                           MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result == 0) mbedtls_ssl_conf_groups(&server->config, server_curves);
    if (result == 0) {
        server->ciphersuites = server_ciphersuites();
        if (server->ciphersuites == NULL) result = -1;
        else {
            mbedtls_ssl_conf_ciphersuites(&server->config, server->ciphersuites);
            mbedtls_ssl_conf_preference_order(&server->config, MBEDTLS_SSL_SRV_CIPHERSUITE_ORDER_SERVER);
        }
    }
    if (result == 0) mbedtls_ssl_conf_session_cache(&server->config, &server->cache,
                                                     mbedtls_ssl_cache_get, mbedtls_ssl_cache_set);
    if (result == 0) result = mbedtls_ssl_conf_own_cert(&server->config, &server->certificate, &server->private_key);
    if (result != 0) { mbedtls_ssl_cache_free(&server->cache); mbedtls_ssl_config_free(&server->config); mbedtls_x509_crt_free(&server->certificate); mbedtls_pk_free(&server->private_key); free(server->ciphersuites); free(server); return -1; }
    *output_server = server;
    return 0;
}

void microhad_tls_server_destroy(void* opaque_server) {
    struct microhad_tls_server* server = (struct microhad_tls_server*)opaque_server;
    if (server == 0) return;
    mbedtls_ssl_cache_free(&server->cache); mbedtls_ssl_config_free(&server->config);
    mbedtls_x509_crt_free(&server->certificate); mbedtls_pk_free(&server->private_key);

    free(server->ciphersuites); free(server);
}

/* These sessions use blocking sockets with SO_RCVTIMEO/SO_SNDTIMEO.
 * Mbed TLS maps EAGAIN to WANT_READ/WRITE, which would retry forever after
 * each socket timeout. Propagate an actual timeout instead. */
static int socket_receive(void* context, unsigned char* bytes, size_t length) {
    int result;
    do { result = mbedtls_net_recv(context, bytes, length); }
    while (result == MBEDTLS_ERR_SSL_WANT_READ && errno == EINTR);
    return result == MBEDTLS_ERR_SSL_WANT_READ ? MBEDTLS_ERR_SSL_TIMEOUT : result;
}
static int socket_send(void* context, const unsigned char* bytes, size_t length) {
    int result;
    do { result = mbedtls_net_send(context, bytes, length); }
    while (result == MBEDTLS_ERR_SSL_WANT_WRITE && errno == EINTR);
    return result == MBEDTLS_ERR_SSL_WANT_WRITE ? MBEDTLS_ERR_SSL_TIMEOUT : result;
}

int microhad_tls_session_open(void* opaque_server, int fd, void** output_session) {
    struct microhad_tls_session* session;
    int result = -1;
    int received;
    struct microhad_tls_server* server = (struct microhad_tls_server*)opaque_server;
    if (server == 0 || output_session == 0) return -1;
    *output_session = 0;
    session = (struct microhad_tls_session*)calloc(1, sizeof(*session));
    if (session == 0) return -1;
    mbedtls_net_init(&session->socket); mbedtls_ssl_init(&session->ssl);
    session->socket.fd = fd;
    session->server = server;
    result = mbedtls_ssl_setup(&session->ssl, &server->config);
    if (result == 0) mbedtls_ssl_set_bio(&session->ssl, &session->socket, socket_send, socket_receive, 0);
    while (result == 0 && (received = mbedtls_ssl_handshake(&session->ssl)) != 0) {
        if (received != MBEDTLS_ERR_SSL_WANT_READ && received != MBEDTLS_ERR_SSL_WANT_WRITE) result = received;
    }
    if (result == 0) { *output_session = session; return 0; }
    release_session(session);
    return -1;
}

int microhad_tls_write(void* opaque_session, const char* response, unsigned long length) {
    struct microhad_tls_session* session = (struct microhad_tls_session*)opaque_session;
    unsigned long offset = 0;
    int result = 0;
    while (offset < length) {
        result = mbedtls_ssl_write(&session->ssl, (const unsigned char*)response + offset, length - offset);
        if (result > 0) offset += (unsigned long)result;
        else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) break;
    }
    return result < 0 ? result : 0;
}

/* Relay callers wait for readability on the underlying socket first, then use
 * this bounded single-record read to move opaque WebSocket bytes. */
int microhad_tls_read(void* opaque_session, char* output, unsigned long capacity) {
    struct microhad_tls_session* session = (struct microhad_tls_session*)opaque_session;
    int received;
    if (session == 0 || output == 0 || capacity == 0) return -1;
    do { received = mbedtls_ssl_read(&session->ssl, (unsigned char*)output, capacity); }
    while (received == MBEDTLS_ERR_SSL_WANT_READ || received == MBEDTLS_ERR_SSL_WANT_WRITE);
    return received;
}

int microhad_tls_pending(void* opaque_session) {
    struct microhad_tls_session* session = (struct microhad_tls_session*)opaque_session;
    return session != 0 && mbedtls_ssl_check_pending(&session->ssl);
}

void microhad_tls_session_close(void* opaque_session) {
    struct microhad_tls_session* session = (struct microhad_tls_session*)opaque_session;
    if (session == 0) return;
    /* Best effort only: closing must not block the idle scheduler. */
    int flags = fcntl(session->socket.fd, F_GETFL, 0);
    if (flags >= 0) fcntl(session->socket.fd, F_SETFL, flags | O_NONBLOCK);
    mbedtls_ssl_close_notify(&session->ssl);
    release_session(session);
}
