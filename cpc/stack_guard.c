/* Stock RSDK has no libssp. Supply a randomized global guard for GCC 4.4's
 * stack-protector ABI; this translation unit is compiled without protection.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

uintptr_t __stack_chk_guard;
__attribute__((noreturn)) void __stack_chk_fail(void);

__attribute__((noreturn)) void __stack_chk_fail(void)
{
    static const char message[] = "CPC experiment stack check failed\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(127);
}

__attribute__((constructor)) static void initialize_stack_guard(void)
{
    unsigned char *guard = (unsigned char *)&__stack_chk_guard;
    unsigned int offset = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        _exit(125);
    while (offset < sizeof(__stack_chk_guard)) {
        ssize_t count = read(fd, guard + offset, sizeof(__stack_chk_guard) - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            _exit(125);
        offset += (unsigned int)count;
    }
    close(fd);
    guard[0] = 0; /* Terminator byte; keep the remaining random bytes private. */
}
