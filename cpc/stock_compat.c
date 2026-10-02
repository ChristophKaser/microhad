#include "stock_compat.h"
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

int epoll_create1(int flags) { return (int)syscall(SYS_epoll_create1, flags); }
int eventfd(unsigned int value, int flags) { return (int)syscall(SYS_eventfd2, value, flags); }
int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags)
{ return (int)syscall(SYS_accept4, fd, address, length, flags); }
int timerfd_create(clockid_t clock, int flags)
{ return (int)syscall(SYS_timerfd_create, clock, flags); }
int timerfd_settime(int fd, int flags, const struct itimerspec *value, struct itimerspec *old)
{ return (int)syscall(SYS_timerfd_settime, fd, flags, value, old); }
int timerfd_gettime(int fd, struct itimerspec *value)
{ return (int)syscall(SYS_timerfd_gettime, fd, value); }
int signalfd(int fd, const sigset_t *mask, int flags)
{ return (int)syscall(SYS_signalfd4, fd, mask, _NSIG / 8, flags); }

int pthread_setname_np(pthread_t thread, const char *name)
{
    int saved_errno, result, error;
    /* Thread names are diagnostic only. Stock LinuxThreads cannot name another
     * thread by pthread_t, so naming that thread is intentionally skipped.
     */
    if (!pthread_equal(thread, pthread_self()))
        return 0;
    if (strlen(name) >= 16)
        return ERANGE;
    saved_errno = errno;
    result = (int)syscall(SYS_prctl, PR_SET_NAME, name, 0, 0, 0);
    error = result == -1 ? errno : 0;
    errno = saved_errno;
    return error;
}
