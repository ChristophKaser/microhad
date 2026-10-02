#define _GNU_SOURCE
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

static void report_illegal_instruction(int signal_number, siginfo_t *info,
                                      void *context)
{
    char message[] = "SIGILL pc=0x00000000\n";
    static const char hex[] = "0123456789abcdef";
    unsigned long pc = (unsigned long)((ucontext_t *)context)->uc_mcontext.pc;
    unsigned int i;
    (void)signal_number;
    (void)info;
    for (i = 0; i < 8; ++i)
        message[12 + i] = hex[(pc >> ((7 - i) * 4)) & 15];
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(132);
}

/* Install before Rust's std startup, so early faults remain diagnosable.
 * Experiment executables built with stock-compat include this object.
 */
__attribute__((constructor)) static void install_fault_report(void)
{
    struct sigaction action;
    action.sa_sigaction = report_illegal_instruction;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGILL, &action, 0) != 0)
        _exit(125);
}
