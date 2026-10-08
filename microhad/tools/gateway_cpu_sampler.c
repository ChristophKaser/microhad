#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Build with the gateway's MIPS cross compiler, for example:
 * mips-linux-gcc -O2 -static -o /tmp/gateway_cpu_sampler tools/gateway_cpu_sampler.c
 * Run as root on the device: /tmp/gateway_cpu_sampler TID [samples]
 *
 * Each sample briefly attaches to the selected thread, reads its saved MIPS
 * EPC, detaches, and waits 10 ms. This avoids restarting the service.
 */
/* Linux/MIPS PTRACE_GETREGS uses a packed 64-bit user register array. Unlike
 * the kernel's internal struct pt_regs, it has no six-word stack pad: GPRs
 * occupy slots 0..31, LO/HI slots 32..33, and EPC slot 34. The slots remain
 * 64 bits wide for a 32-bit MIPS task.
 */
struct mips_regs {
    unsigned long long words[38];
};

int main(int argc, char **argv) {
    pid_t tid;
    int samples = 100;
    int i, j, status, used = 0;
    struct mips_regs regs;
    unsigned long long pcs[1000];
    unsigned int counts[1000];

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s TID [samples]\n", argv[0]);
        return 2;
    }
    tid = (pid_t)atoi(argv[1]);
    if (argc == 3) samples = atoi(argv[2]);
    if (tid <= 0 || samples < 1 || samples > 1000) {
        fprintf(stderr, "TID must be positive and samples must be 1..1000\n");
        return 2;
    }

    for (i = 0; i < 1000; ++i) {
        pcs[i] = 0;
        counts[i] = 0;
    }
    for (i = 0; i < samples; ++i) {
        if (ptrace(PTRACE_ATTACH, tid, 0, 0) < 0) {
            perror("PTRACE_ATTACH");
            return 1;
        }
        if (waitpid(tid, &status, __WALL) < 0) {
            perror("waitpid");
            ptrace(PTRACE_DETACH, tid, 0, 0);
            return 1;
        }
        for (j = 0; j < 38; ++j) regs.words[j] = 0;
        if (ptrace(PTRACE_GETREGS, tid, 0, &regs) < 0) {
            perror("PTRACE_GETREGS");
            ptrace(PTRACE_DETACH, tid, 0, 0);
            return 1;
        }
        if (regs.words[34] == 0) {
            fprintf(stderr, "PTRACE_GETREGS returned EPC=0; refusing invalid sample\n");
            ptrace(PTRACE_DETACH, tid, 0, 0);
            return 1;
        }
        for (j = 0; j < used && pcs[j] != regs.words[34]; ++j) {}
        if (j == used && used < 1000) {
            pcs[used] = regs.words[34];
            counts[used] = 0;
            ++used;
        }
        if (j < used) ++counts[j];
        if (ptrace(PTRACE_DETACH, tid, 0, 0) < 0) {
            perror("PTRACE_DETACH");
            return 1;
        }
        usleep(10000);
    }
    printf("samples=%d unique_pc=%d\n", samples, used);
    for (i = 0; i < used; ++i) printf("0x%llx %u\n", pcs[i], counts[i]);
    return 0;
}
