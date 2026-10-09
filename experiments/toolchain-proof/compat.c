/* Compile only convenience wrappers absent from the selected libc archives. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifdef PROOF_NEED_bcmp
int bcmp(const void *left, const void *right, size_t count) {
    return memcmp(left, right, count);
}
#endif

/* LinuxThreads lacks these GNU stack-inspection APIs. Report that limitation
 * honestly: Rust std handles this error by skipping stack-bound diagnostics.
 * Do not invent a successful result or a guessed stack address.
 */
#ifdef PROOF_NEED_pthread_getattr_np
int pthread_getattr_np(pthread_t thread, pthread_attr_t *attributes) {
    (void)thread;
    (void)attributes;
    return ENOSYS;
}
#endif
#ifdef PROOF_NEED_pthread_attr_getstack
int pthread_attr_getstack(const pthread_attr_t *attributes, void **address, size_t *size) {
    (void)attributes;
    (void)address;
    (void)size;
    return ENOSYS;
}
#endif

#ifdef PROOF_NEED_epoll_create1
int epoll_create1(int flags) { return (int)syscall(SYS_epoll_create1, flags); }
#endif
#ifdef PROOF_NEED_eventfd
int eventfd(unsigned int initial, int flags) { return (int)syscall(SYS_eventfd2, initial, flags); }
#endif
#ifdef PROOF_NEED_accept4
int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags) {
    return (int)syscall(SYS_accept4, fd, address, length, flags);
}
#endif
#ifdef PROOF_NEED_clock_nanosleep
int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remaining) {
    int saved = errno;
    long result = syscall(SYS_clock_nanosleep, clock, flags, request, remaining);
    int error = result == -1 ? errno : 0;
    errno = saved;
    return error;
}
#endif
#ifdef PROOF_NEED_pthread_setname_np
int pthread_setname_np(pthread_t thread, const char *name) {
    int saved, result, error;
    if (!pthread_equal(thread, pthread_self())) return ENOSYS;
    if (strlen(name) >= 16) return ERANGE;
    saved = errno;
    result = (int)syscall(SYS_prctl, PR_SET_NAME, name, 0, 0, 0);
    error = result == -1 ? errno : 0;
    errno = saved;
    return error;
}
#endif
