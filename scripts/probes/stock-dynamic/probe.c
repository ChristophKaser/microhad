#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <unistd.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int value;
static void *worker(void *ignored)
{
    (void)ignored;
    if (pthread_mutex_lock(&lock)) return (void *)1;
    value = 42;
    return pthread_mutex_unlock(&lock) ? (void *)1 : 0;
}
int main(void)
{
    pthread_t thread;
    struct timespec before, after;
    struct ifaddrs *interfaces = 0;
    int pair[2];
    void *result = 0;
    char byte = 0;
    void *memory = malloc(128);
    if (!memory || clock_gettime(CLOCK_MONOTONIC, &before)) return 1;
    if (pthread_create(&thread, 0, worker, 0) || pthread_join(thread, &result) || result || value != 42) return 2;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 3;
    if (write(pair[0], "x", 1) != 1 || read(pair[1], &byte, 1) != 1 || byte != 'x') return 4;
    close(pair[0]); close(pair[1]);
    if (getifaddrs(&interfaces)) return 5;
    freeifaddrs(interfaces);
    if (clock_gettime(CLOCK_MONOTONIC, &after) || after.tv_sec < before.tv_sec) return 6;
    free(memory);
    puts("PASS C: allocation, pthread/mutex, clocks, sockets, interfaces");
    return 0;
}
