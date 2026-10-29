#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <unistd.h>

#if CRASH_SNAPSHOTS_BUILD
extern void snapshot_fault(int, void *, void *);
#endif

static unsigned char fault_stack[64 * 1024] __attribute__((aligned(16)));

static char *text(char *out, const char *value)
{
    while (*value) *out++ = *value++;
    return out;
}

static char *hex_value(char *out, uintptr_t value)
{
    static const char hex[] = "0123456789abcdef";
    unsigned int i;
    for (i = 0; i < sizeof(value) * 2; ++i)
        *out++ = hex[(value >> ((sizeof(value) * 2 - 1 - i) * 4)) & 15];
    return out;
}

static void report_fault(int number, siginfo_t *info, void *context)
{
    ucontext_t *state = (ucontext_t *)context;
    uintptr_t pc = 0, ra = 0, sp = 0;
    char message[256];
    char *out = message;
    long tid = syscall(SYS_gettid);
#if defined(__mips__)
    pc = (uintptr_t)state->uc_mcontext.pc;
    ra = (uintptr_t)state->uc_mcontext.gregs[31];
    sp = (uintptr_t)state->uc_mcontext.gregs[29];
#elif defined(__x86_64__)
    /* Host regression fixture exercises the same handler and signal delivery. */
    pc = (uintptr_t)state->uc_mcontext.gregs[REG_RIP];
    sp = (uintptr_t)state->uc_mcontext.gregs[REG_RSP];
#else
#error Unsupported fault-report context
#endif
    out = text(out, number == SIGSEGV ? "SIGSEGV pc=0x" : "SIGILL pc=0x");
    out = hex_value(out, pc);
    out = text(out, " ra=0x"); out = hex_value(out, ra);
    out = text(out, " sp=0x"); out = hex_value(out, sp);
    out = text(out, " addr=0x"); out = hex_value(out, (uintptr_t)info->si_addr);
    out = text(out, " code=0x"); out = hex_value(out, (uintptr_t)info->si_code);
    out = text(out, " tid=0x"); out = hex_value(out, (uintptr_t)tid);
    *out++ = '\n';
    /* No allocation, formatting library, locks or syslog in a signal handler.
     * The supervisor's stderr FIFO sends this bounded record to host syslog.
     */
    (void)write(STDERR_FILENO, message, (size_t)(out - message));
    #if CRASH_SNAPSHOTS_BUILD
    snapshot_fault(number, info, context);
    #endif
    /* Return to a synchronous fault so the default action captures its original
     * registers, rather than the handler's re-delivery syscall. */
    if (info->si_code > 0) return;
    /* SA_RESETHAND restored SIG_DFL. Re-deliver to this kernel thread so the
     * kernel can log the fatal signal and the parent sees a signal death.
     * Raw kernel IDs also work with stock LinuxThreads' distinct task IDs.
     */
    if (syscall(SYS_tgkill, syscall(SYS_getpid), tid, number) != 0)
        _exit(128 + number);
}

/* Install before Rust startup; keep the main thread's stack faults reportable.
 * Kernel fatal-signal logging also covers faults on other threads' bad stacks.
 */
__attribute__((constructor)) static void install_fault_report(void)
{
    stack_t stack;
    struct sigaction action;
    memset(&stack, 0, sizeof(stack));
    memset(&action, 0, sizeof(action));
    stack.ss_sp = fault_stack;
    stack.ss_size = sizeof(fault_stack);
    if (sigaltstack(&stack, 0) != 0) _exit(125);
    action.sa_sigaction = report_fault;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
    if (sigaction(SIGILL, &action, 0) != 0 || sigaction(SIGSEGV, &action, 0) != 0)
        _exit(125);
}
