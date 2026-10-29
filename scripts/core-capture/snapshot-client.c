#define _GNU_SOURCE
#include "snapshot.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <sys/un.h>
#include <unistd.h>

static struct snap_job *job;
static int notify=-1;
static pthread_mutex_t registry_lock=PTHREAD_MUTEX_INITIALIZER;
struct start { void *(*entry)(void *);void *arg; };
struct thread_cleanup { int slot;void *stack; };
extern int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
static int register_thread(void) {
    int i,slot=-1,pid=syscall(SYS_gettid);unsigned long start=snapshot_start(pid);
    pthread_mutex_lock(&registry_lock);
    for(i=0;i<SNAP_MAX_THREADS;i++)if(job->threads[i].pid==0){
        job->threads[i].start=start;job->threads[i].pid=pid;slot=i;break;
    }
    pthread_mutex_unlock(&registry_lock);return slot;
}
static void cleanup_thread(void *arg) {
    struct thread_cleanup *c=arg;
    if(c->slot>=0){pthread_mutex_lock(&registry_lock);job->threads[c->slot].pid=0;pthread_mutex_unlock(&registry_lock);}
    if(c->stack!=MAP_FAILED){stack_t stack;memset(&stack,0,sizeof(stack));stack.ss_flags=SS_DISABLE;sigaltstack(&stack,0);munmap(c->stack,65536);}
}
static void *thread_entry(void *arg) {
    struct start copy=*(struct start *)arg;struct thread_cleanup c;void *result;stack_t stack;
    free(arg);c.slot=register_thread();c.stack=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(c.stack!=MAP_FAILED){memset(&stack,0,sizeof(stack));stack.ss_sp=c.stack;stack.ss_size=65536;sigaltstack(&stack,0);}
    pthread_cleanup_push(cleanup_thread,&c);
    result=copy.entry(copy.arg);
    pthread_cleanup_pop(1);
    return result;
}
int __wrap_pthread_create(pthread_t *thread,const pthread_attr_t *attr,void *(*entry)(void *),void *arg) {
    struct start *s;int result;
    if(!job)return __real_pthread_create(thread,attr,entry,arg);
    if(job->armed)return EAGAIN;
    s=malloc(sizeof(*s));if(!s)return ENOMEM;s->entry=entry;s->arg=arg;
    result=__real_pthread_create(thread,attr,thread_entry,s);if(result)free(s);return result;
}
__attribute__((constructor)) static void snapshot_register(void) {
    struct sockaddr_un address;struct stat st;char path[96];int fd;
    if(lstat("/tmp/gateway-snapshots",&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077))return;
    notify=socket(AF_UNIX,SOCK_DGRAM,0);if(notify<0)return;
    memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;strcpy(address.sun_path,"/tmp/gateway-snapshots/notify");
    if(connect(notify,(struct sockaddr *)&address,sizeof(address))){close(notify);notify=-1;return;}
    fcntl(notify,F_SETFL,O_NONBLOCK);fcntl(notify,F_SETFD,FD_CLOEXEC);
    snprintf(path,sizeof(path),"/tmp/gateway-snapshots/job.%ld",syscall(SYS_getpid));
    fd=open(path,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)return;
    if(ftruncate(fd,sizeof(struct snap_job))){close(fd);unlink(path);return;}
    job=mmap(0,sizeof(*job),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);
    if(job==MAP_FAILED){job=0;unlink(path);return;}
    memset(job,0,sizeof(*job));job->owner.pid=syscall(SYS_getpid);job->owner.start=snapshot_start(job->owner.pid);job->magic=SNAP_MAGIC;
}
void snapshot_fault(int number,void *info,void *context) {
    siginfo_t *si=info;ucontext_t *u=context;int i;
    if(!job||notify<0)return;
    if(job->armed)return;
    {int tid=syscall(SYS_gettid),known=tid==job->owner.pid;
     for(i=0;i<SNAP_MAX_THREADS;i++)if(job->threads[i].pid==tid)known=1;
     if(!known)return;}

    job->signal_number=number;job->signal_code=si->si_code;job->fault_tid=syscall(SYS_gettid);job->fault_address=(unsigned long)si->si_addr;
    for(i=0;i<32;i++)job->regs[i]=u->uc_mcontext.gregs[i];
    job->regs[32]=u->uc_mcontext.mdlo;job->regs[33]=u->uc_mcontext.mdhi;job->regs[34]=u->uc_mcontext.pc;
    __asm__ __volatile__("" ::: "memory");job->armed=1;
    /* A failed notification must not strand a task. The watchdog also polls
     * armed shared state, including if the collector dies before notification. */
    /* LinuxThreads' write cancellation wrapper cannot identify a thread from
     * an alternate stack. The handler must use the kernel entry directly. */
    if(syscall(SYS_write,notify,"!",1)!=1){job->armed=0;return;}
    syscall(SYS_kill,job->fault_tid,SIGSTOP);
}
