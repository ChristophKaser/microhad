#ifndef GATEWAY_SNAPSHOT_H
#define GATEWAY_SNAPSHOT_H
#include <stdint.h>
#define SNAP_MAX_THREADS 128
#define SNAP_MAGIC 0x534e4150
struct snap_task { int pid; unsigned long start; };
struct snap_job {
    uint32_t magic;
    struct snap_task owner;
    struct snap_task collector;
    volatile int armed;
    int signal_number, signal_code, fault_tid;
    unsigned long fault_address;
    uint64_t regs[38];
    struct snap_task threads[SNAP_MAX_THREADS];
};
void snapshot_fault(int number, void *info, void *context);
int snapshot_write(struct snap_job *job);
int snapshot_tasks(struct snap_job *job, struct snap_task *tasks);
unsigned long snapshot_start(int pid);
void snapshot_kill(struct snap_task *tasks, int count);
#endif
