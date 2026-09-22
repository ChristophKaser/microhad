#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cache.h>
#include <mbedtls/x509_crt.h>
#include <pthread.h>

struct microhad_tls_server {
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context private_key;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_ssl_cache_context cache;
    pthread_mutex_t rng_mutex;
};

struct microhad_tls_session {
    mbedtls_net_context socket;
    mbedtls_ssl_context ssl;
    struct microhad_tls_server* server;
};

/* P-521 ECDHE costs roughly two seconds on the gateway's MIPS CPU. P-256 is
 * widely supported by browsers and is also the curve used for Web Push. */
static const mbedtls_ecp_group_id server_curves[] = {
    MBEDTLS_ECP_DP_SECP256R1,
    MBEDTLS_ECP_DP_NONE
};

static char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

static int header_name_is(const char* start, const char* end, const char* name) {
    unsigned long length = (unsigned long)(end - start);
    unsigned long i;
    if (length != strlen(name)) return 0;
    for (i = 0; i < length; i++) if (ascii_lower(start[i]) != name[i]) return 0;
    return 1;
}

static int request_length(char* request, unsigned long capacity, unsigned long* expected) {
    char* header_end = strstr(request, "\r\n\r\n");
    char* line = strstr(request, "\r\n");
    unsigned long content_length = 0;
    int saw_content_length = 0;
    if (header_end == 0 || line == 0 || line == request) return -1;
    if ((unsigned long)(header_end + 4 - request) >= capacity) return -2;
    line += 2;
    while (line < header_end) {
        char* line_end = strstr(line, "\r\n");
        char* colon;
        char* value;
        char* end;
        unsigned long parsed;
        if (line_end == 0 || line_end > header_end || (colon = strchr(line, ':')) == 0 || colon >= line_end) return -1;
        value = colon + 1;
        while (value < line_end && (*value == ' ' || *value == '\t')) value++;
        if (header_name_is(line, colon, "transfer-encoding")) return -1;
        if (header_name_is(line, colon, "content-length")) {
            if (value == line_end || *value == '-') return -1;
            errno = 0;
            parsed = strtoul(value, &end, 10);
            if (errno != 0 || end == value || end != line_end || (saw_content_length && parsed != content_length)) return -1;
            saw_content_length = 1;
            content_length = parsed;
        }
        line = line_end + 2;
    }
    if (content_length > capacity - 1 - (unsigned long)(header_end + 4 - request)) return -2;
    *expected = (unsigned long)(header_end + 4 - request) + content_length;
    return 0;
}

static void release_session(struct microhad_tls_session* session) {
    if (session == 0) return;
    mbedtls_ssl_free(&session->ssl);
    /* HttpServer owns the descriptor and closes it after this TLS session. */
    session->socket.fd = -1;
    mbedtls_net_free(&session->socket); free(session);
}

static int locked_random(void* opaque, unsigned char* output, size_t length) {
    struct microhad_tls_server* server = (struct microhad_tls_server*)opaque;
    int result;
    pthread_mutex_lock(&server->rng_mutex);
    result = mbedtls_ctr_drbg_random(&server->drbg, output, length);
    pthread_mutex_unlock(&server->rng_mutex);
    return result;
}

static int locked_cache_get(void* opaque, mbedtls_ssl_session* session) {
    struct microhad_tls_server* server = (struct microhad_tls_server*)opaque;
    int result;
    pthread_mutex_lock(&server->rng_mutex);
    result = mbedtls_ssl_cache_get(&server->cache, session);
    pthread_mutex_unlock(&server->rng_mutex);
    return result;
}

static int locked_cache_set(void* opaque, const mbedtls_ssl_session* session) {
    struct microhad_tls_server* server = (struct microhad_tls_server*)opaque;
    int result;
    pthread_mutex_lock(&server->rng_mutex);
    result = mbedtls_ssl_cache_set(&server->cache, session);
    pthread_mutex_unlock(&server->rng_mutex);
    return result;
}

int microhad_tls_server_create(const char* certificate_path, const char* private_key_path, void** output_server) {
    struct microhad_tls_server* server;
    int result;
    if (output_server == 0) return -1;
    *output_server = 0;
    server = (struct microhad_tls_server*)calloc(1, sizeof(*server));
    if (server == 0) return -1;
    mbedtls_ssl_config_init(&server->config); mbedtls_x509_crt_init(&server->certificate);
    mbedtls_pk_init(&server->private_key); mbedtls_ctr_drbg_init(&server->drbg);
    mbedtls_entropy_init(&server->entropy); mbedtls_ssl_cache_init(&server->cache);
    if (pthread_mutex_init(&server->rng_mutex, 0) != 0) { free(server); return -1; }
    result = mbedtls_ctr_drbg_seed(&server->drbg, mbedtls_entropy_func, &server->entropy,
                                   (const unsigned char*)"microhad-web-server", 15);
    if (result == 0) result = mbedtls_x509_crt_parse_file(&server->certificate, certificate_path);
    if (result == 0) result = mbedtls_pk_parse_keyfile(&server->private_key, private_key_path, 0);
    if (result == 0) result = mbedtls_ssl_config_defaults(&server->config, MBEDTLS_SSL_IS_SERVER,
                                                           MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result == 0) mbedtls_ssl_conf_curves(&server->config, server_curves);
    if (result == 0) mbedtls_ssl_conf_rng(&server->config, locked_random, server);
    if (result == 0) mbedtls_ssl_conf_session_cache(&server->config, server,
                                                     locked_cache_get, locked_cache_set);
    if (result == 0) result = mbedtls_ssl_conf_own_cert(&server->config, &server->certificate, &server->private_key);
    if (result != 0) { mbedtls_ssl_cache_free(&server->cache); mbedtls_ssl_config_free(&server->config); mbedtls_x509_crt_free(&server->certificate); mbedtls_pk_free(&server->private_key); mbedtls_ctr_drbg_free(&server->drbg); mbedtls_entropy_free(&server->entropy); pthread_mutex_destroy(&server->rng_mutex); free(server); return -1; }
    *output_server = server;
    return 0;
}

void microhad_tls_server_destroy(void* opaque_server) {
    struct microhad_tls_server* server = (struct microhad_tls_server*)opaque_server;
    if (server == 0) return;
    mbedtls_ssl_cache_free(&server->cache); mbedtls_ssl_config_free(&server->config);
    mbedtls_x509_crt_free(&server->certificate); mbedtls_pk_free(&server->private_key);
    mbedtls_ctr_drbg_free(&server->drbg); mbedtls_entropy_free(&server->entropy);
    pthread_mutex_destroy(&server->rng_mutex); free(server);
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
    if (result == 0) mbedtls_ssl_set_bio(&session->ssl, &session->socket, mbedtls_net_send, mbedtls_net_recv, 0);
    while (result == 0 && (received = mbedtls_ssl_handshake(&session->ssl)) != 0) {
        if (received != MBEDTLS_ERR_SSL_WANT_READ && received != MBEDTLS_ERR_SSL_WANT_WRITE) result = received;
    }
    if (result == 0) { *output_session = session; return 0; }
    release_session(session);
    return -1;
}

int microhad_tls_read_request(void* opaque_session, char* request, unsigned long capacity) {
    struct microhad_tls_session* session = (struct microhad_tls_session*)opaque_session;
    int result = 0;
    int received;
    unsigned long length = 0;
    unsigned long expected = 0;
    int parsed_headers = 0;
    if (session == 0 || capacity < 2) return -1;
    while (result == 0 && length < capacity - 1) {
        received = mbedtls_ssl_read(&session->ssl, (unsigned char*)request + length, capacity - 1 - length);
        if (received > 0) {
            length += (unsigned long)received;
            request[length] = 0;
            if (!parsed_headers && strstr(request, "\r\n\r\n") != 0) {
                result = request_length(request, capacity, &expected);
                if (result != 0) break;
                parsed_headers = 1;
            }
            if (parsed_headers && length >= expected) { request[expected] = 0; break; }
        }
        else if (received != MBEDTLS_ERR_SSL_WANT_READ && received != MBEDTLS_ERR_SSL_WANT_WRITE) { result = -1; }
    }
    if (result == -2 || (!parsed_headers && length == capacity - 1)) return -2;
    return result == 0 && parsed_headers ? 0 : -1;
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

void microhad_tls_session_close(void* opaque_session) {
    struct microhad_tls_session* session = (struct microhad_tls_session*)opaque_session;
    if (session == 0) return;
    mbedtls_ssl_close_notify(&session->ssl);
    release_session(session);
}
