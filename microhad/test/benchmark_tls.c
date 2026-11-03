/* Isolated synthetic TLS server. No gateway configuration or persistent state. */
#define _POSIX_C_SOURCE 200809L
#define _BSD_SOURCE 1
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <netinet/in.h>
#include "../src/tls_server.c"

/* The benchmark accepts bodyless GETs only; production HTTP framing lives in
 * HttpServer and is shared by plaintext and TLS connections. */
static int read_get_request(void* session, char* request, size_t capacity) {
    size_t length = 0;
    while (length + 1 < capacity) {
        int count = microhad_tls_read(session, request + length, capacity - length - 1);
        if (count <= 0) return -1;
        length += (size_t)count; request[length] = 0;
        if (strstr(request, "\r\n\r\n") != NULL) return strncmp(request, "GET ", 4) == 0 ? 0 : -1;
    }
    return -1;
}

static int cache_hit;
static double cpu_us(void) {
    struct timespec t;
    struct rusage r;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) == 0)
        return t.tv_sec * 1000000.0 + t.tv_nsec / 1000.0;
    if (getrusage(RUSAGE_SELF, &r) != 0) exit(2);
    return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000000.0 +
           r.ru_utime.tv_usec + r.ru_stime.tv_usec;
}
static double wall_us(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) exit(2);
    return t.tv_sec * 1000000.0 + t.tv_nsec / 1000.0;
}
static int cached_get(void *data, const unsigned char *id, size_t length,
                      mbedtls_ssl_session *session) {
    int r = mbedtls_ssl_cache_get(data, id, length, session);
    if (r == 0) cache_hit = 1;
    return r;
}
int main(int argc, char **argv) {
    void *opaque = NULL;
    struct microhad_tls_server *server;
    struct sockaddr_in address;
    int listener, suite[2], fd, reuse = 1;
    double cpu, wall;
    if (argc != 5) return 2;
    signal(SIGPIPE, SIG_IGN);
    alarm(600);
    errno = 0;
    if (nice(10) == -1 && errno != 0) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    cpu = cpu_us(); wall = wall_us();
    if (microhad_tls_server_create(argv[1], argv[2], &opaque) != 0) {
        mbedtls_x509_crt cert;
        mbedtls_pk_context key;
        mbedtls_x509_crt_init(&cert); mbedtls_pk_init(&key);
        fprintf(stderr, "TLS init failed: crypto=%d cert=%d key=%d\n", microhad_crypto_init(),
                mbedtls_x509_crt_parse_file(&cert, argv[1]),
                mbedtls_pk_parse_keyfile(&key, argv[2], NULL));
        return 3;
    }
    server = (struct microhad_tls_server *)opaque;
    suite[0] = atoi(argv[3]); suite[1] = 0;
    mbedtls_ssl_conf_ciphersuites(&server->config, suite);
    mbedtls_ssl_conf_session_cache(&server->config, &server->cache,
                                  cached_get, mbedtls_ssl_cache_set);
    printf("{\"event\":\"init\",\"cpu_us\":%.0f,\"wall_us\":%.0f}\n", cpu_us()-cpu, wall_us()-wall);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)atoi(argv[4]));
    /* No SO_REUSEPORT: a collision must fail without affecting another server. */
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 4) != 0) { perror("benchmark bind/listen"); return 4; }
    puts("{\"event\":\"ready\"}");
    {
        struct timespec resolution, check;
        int precise = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &check) == 0;
        if (clock_getres(precise ? CLOCK_PROCESS_CPUTIME_ID : CLOCK_MONOTONIC, &resolution) != 0)
            return 5;
        printf("{\"event\":\"timing\",\"cpu_clock\":\"%s\",\"resolution_ns\":%.0f}\n",
               precise ? "process" : "getrusage", resolution.tv_sec * 1e9 + resolution.tv_nsec);
    }
    while ((fd = accept(listener, NULL, NULL)) >= 0) {
        void *session = NULL;
        struct timeval timeout;
        int result, block = 0, count = 1, i;
        char request[4096], header[512], body[16384];
        timeout.tv_sec = 5; timeout.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        cache_hit = 0; cpu = cpu_us(); wall = wall_us();
        result = microhad_tls_session_open(server, fd, &session);
        printf("{\"event\":\"handshake\",\"ok\":%s,\"cache_hit\":%s,\"cpu_us\":%.0f,\"wall_us\":%.0f,\"cipher\":%d,\"negotiated\":\"%s\"}\n",
               result == 0 ? "true" : "false", cache_hit ? "true" : "false",
               cpu_us()-cpu, wall_us()-wall, suite[0], result == 0 ?
               mbedtls_ssl_get_ciphersuite(&((struct microhad_tls_session *)session)->ssl) : "");
        if (result != 0) { close(fd); continue; }
        if (read_get_request(session, request, sizeof(request)) == 0) {
            if (strncmp(request, "GET /records/1024 ", 18) == 0) block = 1024;
            if (strncmp(request, "GET /records/16384 ", 19) == 0) block = 16384;
            if (block) count = 64; else block = 128;
            memset(body, 'x', sizeof(body));
            snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: %d\r\n\r\n", block * count);
            microhad_tls_write(session, header, strlen(header));
            cpu = cpu_us(); wall = wall_us();
            for (i = 0; i < count; ++i)
                if (microhad_tls_write(session, body, block) != 0) break;
            printf("{\"event\":\"records\",\"ok\":%s,\"bytes\":%d,\"block\":%d,\"cpu_us\":%.0f,\"wall_us\":%.0f}\n",
                   i == count ? "true" : "false", i * block, block, cpu_us()-cpu, wall_us()-wall);
        }
        microhad_tls_session_close(session);
        close(fd);
    }
    close(listener);
    microhad_tls_server_destroy(server);
    return 0;
}
