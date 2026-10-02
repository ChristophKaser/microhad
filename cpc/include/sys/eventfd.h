#ifndef EXPERIMENT_EVENTFD_H
#define EXPERIMENT_EVENTFD_H
#include <fcntl.h>
#include <stdint.h>
#define EFD_CLOEXEC O_CLOEXEC
#define EFD_NONBLOCK O_NONBLOCK
#define EFD_SEMAPHORE 1
typedef uint64_t eventfd_t;
int eventfd(unsigned int initial_value, int flags);
#endif
