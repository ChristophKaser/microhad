/* Keep kernel fatal-signal state authoritative on stock LinuxThreads.
 * Its private handler table is stale after direct __libc_sigaction registration.
 * Rust queries through sigaction during startup and must see the real handler.
 */
#define _GNU_SOURCE
#include <signal.h>
extern int __libc_sigaction(int, const struct sigaction *, struct sigaction *);
extern int __real_sigaction(int, const struct sigaction *, struct sigaction *);
int __wrap_sigaction(int number, const struct sigaction *action, struct sigaction *old)
{
    if (number == SIGSEGV || number == SIGILL || number == SIGBUS)
        return __libc_sigaction(number, action, old);
    return __real_sigaction(number, action, old);
}
