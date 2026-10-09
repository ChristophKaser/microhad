#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x, errno); exit(1); } } while (0)
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static pthread_key_t key;
static int ready;
static int go;
static unsigned int count;
static unsigned int destroyed;

static void destroy_value(void *value) {
    CHECK(value != NULL);
    CHECK(pthread_mutex_lock(&mutex) == 0);
    ++destroyed;
    CHECK(pthread_mutex_unlock(&mutex) == 0);
}

static void *worker(void *value) {
    unsigned int i;
    CHECK(pthread_setspecific(key, value) == 0);
    CHECK(pthread_mutex_lock(&mutex) == 0);
    ++ready;
    CHECK(pthread_cond_broadcast(&condition) == 0);
    while (!go) CHECK(pthread_cond_wait(&condition, &mutex) == 0);
    CHECK(pthread_mutex_unlock(&mutex) == 0);
    for (i = 0; i < 1000; ++i) {
        errno = (int)(uintptr_t)value;
        CHECK(pthread_mutex_lock(&mutex) == 0);
        ++count;
        CHECK(pthread_mutex_unlock(&mutex) == 0);
        CHECK(errno == (int)(uintptr_t)value);
        CHECK(pthread_getspecific(key) == value);
    }
    return value;
}

void proof_c(void) {
    pthread_t threads[2];
    unsigned int i;
    struct timespec before, after, delay;
    int pair[2], listener, client, accepted;
    struct sockaddr_in address;
    struct sockaddr_un unix_address;
    socklen_t length;
    FILE *random;
    unsigned char bytes[32];
    char received;
    CHECK(pthread_key_create(&key, destroy_value) == 0);
    for (i = 0; i < 2; ++i) CHECK(pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)(i + 101)) == 0);
    CHECK(pthread_mutex_lock(&mutex) == 0);
    while (ready != 2) CHECK(pthread_cond_wait(&condition, &mutex) == 0);
    go = 1;
    CHECK(pthread_cond_broadcast(&condition) == 0);
    CHECK(pthread_mutex_unlock(&mutex) == 0);
    for (i = 0; i < 2; ++i) {
        void *result;
        CHECK(pthread_join(threads[i], &result) == 0);
        CHECK(result == (void *)(uintptr_t)(i + 101));
    }
    CHECK(count == 2000 && destroyed == 2);
    CHECK(pthread_getspecific(key) == NULL);
    CHECK(pthread_key_delete(key) == 0);
    puts("PASS C pthreads, mutex, condition, keys, destructors, errno");

    CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    delay.tv_sec = 0; delay.tv_nsec = 20000000;
    CHECK(nanosleep(&delay, NULL) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    CHECK(after.tv_sec > before.tv_sec || (after.tv_sec == before.tv_sec && after.tv_nsec > before.tv_nsec));
    random = fopen("/dev/urandom", "rb");
    CHECK(random != NULL);
    CHECK(fread(bytes, 1, sizeof(bytes), random) == sizeof(bytes));
    CHECK(fclose(random) == 0);
    puts("PASS C monotonic timer, sleep, random-device read");

    listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(listen(listener, 1) == 0);
    length = sizeof(address);
    CHECK(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(client >= 0);
    CHECK(connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
    accepted = accept(listener, NULL, NULL);
    CHECK(accepted >= 0);
    CHECK(write(client, "T", 1) == 1 && read(accepted, &received, 1) == 1 && received == 'T');
    CHECK(close(accepted) == 0 && close(client) == 0 && close(listener) == 0);
    puts("PASS C loopback TCP");

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(write(pair[0], "U", 1) == 1 && read(pair[1], &received, 1) == 1 && received == 'U');
    CHECK(close(pair[0]) == 0 && close(pair[1]) == 0);
    /* Linux abstract namespace: no filesystem or application socket changes. */
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    memset(&unix_address, 0, sizeof(unix_address));
    unix_address.sun_family = AF_UNIX;
    snprintf(unix_address.sun_path + 1, sizeof(unix_address.sun_path) - 1, "compiler-proof-%ld", (long)getpid());
    CHECK(bind(listener, (struct sockaddr *)&unix_address, sizeof(unix_address)) == 0);
    CHECK(listen(listener, 1) == 0);
    client = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(client >= 0);
    CHECK(connect(client, (struct sockaddr *)&unix_address, sizeof(unix_address)) == 0);
    accepted = accept(listener, NULL, NULL);
    CHECK(accepted >= 0);
    CHECK(close(accepted) == 0 && close(client) == 0 && close(listener) == 0);
    puts("PASS C Unix sockets");
}
