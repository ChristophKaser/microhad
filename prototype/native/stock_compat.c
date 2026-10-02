#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Tokio/Mio needs these Linux APIs; the stock kernel supports their syscalls
 * but the old libc archive does not contain the convenience wrappers.
 */
int epoll_create1(int flags)
{
    return (int)syscall(SYS_epoll_create1, flags);
}

int eventfd(unsigned int initial_value, int flags)
{
    return (int)syscall(SYS_eventfd2, initial_value, flags);
}

/* The stock libc lacks these wrappers, although its headers list the syscalls.
 * Preserve real errors; do not substitute a non-atomic accept + fcntl sequence.
 */
int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags)
{
    return (int)syscall(SYS_accept4, fd, address, length, flags);
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                    struct timespec *remaining)
{
    int saved_errno = errno;
    long result = syscall(SYS_clock_nanosleep, clock, flags, request, remaining);
    int error = result == -1 ? errno : 0;
    errno = saved_errno;
    return error; /* Unlike nanosleep, POSIX returns an error number directly. */
}

int pthread_setname_np(pthread_t thread, const char *name)
{
    int saved_errno;
    int result;
    int error;
    if (!pthread_equal(thread, pthread_self()))
        return ENOSYS; /* Only the current thread is supported by this probe. */
    if (strlen(name) >= 16)
        return ERANGE;
    saved_errno = errno;
    result = (int)syscall(SYS_prctl, PR_SET_NAME, name, 0, 0, 0);
    error = result == -1 ? errno : 0;
    errno = saved_errno;
    return error;
}
