#ifndef EXPERIMENT_CPC_STOCK_COMPAT_H
#define EXPERIMENT_CPC_STOCK_COMPAT_H
#include <fcntl.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#ifndef EPOLL_CLOEXEC
#define EPOLL_CLOEXEC O_CLOEXEC
#endif
/* The old timer/signal headers contain generic flags; MIPS uses O_NONBLOCK=128. */
#undef TFD_NONBLOCK
#define TFD_NONBLOCK O_NONBLOCK
#undef SFD_NONBLOCK
#define SFD_NONBLOCK O_NONBLOCK
int epoll_create1(int flags);
int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags);
int pthread_setname_np(pthread_t thread, const char *name);
#endif
