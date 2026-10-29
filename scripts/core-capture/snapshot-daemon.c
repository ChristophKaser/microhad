#define _GNU_SOURCE
#include "snapshot.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#define ROOT "/tmp/gateway-snapshots"
static volatile sig_atomic_t stopping;
static void stop(int number){(void)number;stopping=1;}
static long seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec;}
static void cleanup(struct snap_job *job){if(job->collector.pid>1&&snapshot_start(job->collector.pid)==job->collector.start)kill(-job->collector.pid,SIGKILL);struct snap_task tasks[SNAP_MAX_THREADS+2];int n=snapshot_tasks(job,tasks),i;snapshot_kill(tasks,n);
    for(i=0;i<SNAP_MAX_THREADS;i++)if(job->threads[i].pid>1&&snapshot_start(job->threads[i].pid)==job->threads[i].start)kill(job->threads[i].pid,SIGKILL);
    if(job->owner.pid>1&&snapshot_start(job->owner.pid)==job->owner.start)kill(job->owner.pid,SIGKILL);
}
static void collect(struct snap_job *job,const char *transport){
    int pipefd[2],status,result;pid_t sender;
    setpgid(0,0);
#ifdef SNAPSHOT_TESTING
    if(getenv("SNAPSHOT_TEST_DIE"))_exit(77);
    if(getenv("SNAPSHOT_TEST_PAUSE"))raise(SIGSTOP);
#endif
    signal(SIGTERM,SIG_DFL);signal(SIGINT,SIG_DFL);signal(SIGPIPE,SIG_IGN);
    if(pipe(pipefd))_exit(2);
    sender=fork();if(sender<0)_exit(2);
    if(!sender){char pid[24],sig[24],epoch[32];
        dup2(pipefd[0],0);close(pipefd[0]);close(pipefd[1]);
        snprintf(pid,sizeof(pid),"%d",job->owner.pid);snprintf(sig,sizeof(sig),"%d",job->signal_number);snprintf(epoch,sizeof(epoch),"%lu",(unsigned long)time(0));
        execl(transport,transport,"--snapshot",pid,sig,epoch,(char *)0);_exit(127);
    }
    close(pipefd[0]);dup2(pipefd[1],1);close(pipefd[1]);
    result=snapshot_write(job);close(1);
    waitpid(sender,&status,0);
    /* Snapshot writing errors must never be reported as complete by transport.
     * A separate result marker below is added by the transport's ELF validator. */
    _exit(result?2:0);
}
int main(int argc,char **argv){
    int fd,lock;struct sockaddr_un address;char transport[1024],path[256];
    pid_t worker=0,retry=0;long deadline=0,next_retry=0;struct snap_job active;char active_path[256]="";
    if(argc!=2)return 2;
    if(strlen(argv[1])>=sizeof(transport))return 2;
    strcpy(transport,argv[1]);
    umask(077);openlog("gateway-snapshot",LOG_PID,LOG_DAEMON);
    if(mkdir(ROOT,0700)&&errno!=EEXIST)return 2;
    {struct stat st;if(lstat(ROOT,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077))return 2;}
    lock=open(ROOT "/daemon.lock",O_RDWR|O_CREAT|O_NOFOLLOW,0600);if(lock<0||flock(lock,LOCK_EX|LOCK_NB))return 2;
    if(!strcmp(argv[1],"--recover")){
        DIR *d=opendir(ROOT);struct dirent *e;
        if(d){while((e=readdir(d))){struct snap_job job;int f;
            if(strncmp(e->d_name,"job.",4))continue;
            snprintf(path,sizeof(path),ROOT "/%s",e->d_name);f=open(path,O_RDONLY|O_NOFOLLOW);
            if(f>=0){if(read(f,&job,sizeof(job))==sizeof(job)&&job.magic==SNAP_MAGIC&&job.armed)cleanup(&job);close(f);}
        }closedir(d);}return 0;
    }
    fd=socket(AF_UNIX,SOCK_DGRAM,0);if(fd<0)return 2;
    memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;strcpy(address.sun_path,ROOT "/notify");unlink(address.sun_path);
    if(bind(fd,(struct sockaddr *)&address,sizeof(address)))return 2;
    fcntl(fd,F_SETFD,FD_CLOEXEC);fcntl(lock,F_SETFD,FD_CLOEXEC);
    {DIR *d=opendir(ROOT);struct dirent *e;
     if(d){while((e=readdir(d))){struct snap_job job;int f;
      if(strncmp(e->d_name,"job.",4))continue;
      snprintf(path,sizeof(path),ROOT "/%s",e->d_name);f=open(path,O_RDONLY|O_NOFOLLOW);
      if(f>=0){if(read(f,&job,sizeof(job))==sizeof(job)&&job.magic==SNAP_MAGIC&&job.armed){cleanup(&job);unlink(path);}close(f);}
     }closedir(d);}}
    signal(SIGTERM,stop);signal(SIGINT,stop);
    {FILE *ready=fopen(ROOT "/daemon.pid","w");if(!ready)return 2;fprintf(ready,"%ld\n",(long)getpid());fclose(ready);}
    syslog(LOG_INFO,"userspace crash collector ready");
    while(!stopping){
        DIR *d;struct dirent *e;struct pollfd p={fd,POLLIN,0};
        if(worker){int status;pid_t ended=waitpid(worker,&status,WNOHANG);
            if(ended==worker||seconds()>=deadline){
                if(ended!=worker)kill(-worker,SIGKILL);
                cleanup(&active);unlink(active_path);waitpid(worker,&status,0);
                syslog(LOG_WARNING,"snapshot collection finished pid=%d status=%d",active.owner.pid,status);worker=0;
            }
        }
        d=opendir(ROOT);if(!d)break;
        while((e=readdir(d))){struct snap_job job;struct stat st;int f;
            if(strncmp(e->d_name,"job.",4))continue;
            snprintf(path,sizeof(path),ROOT "/%s",e->d_name);
            f=open(path,O_RDONLY|O_NOFOLLOW);if(f<0)continue;
            if(fstat(f,&st)||st.st_size!=sizeof(job)||read(f,&job,sizeof(job))!=sizeof(job)){close(f);continue;}close(f);
            if(job.magic!=SNAP_MAGIC)continue;
            if(snapshot_start(job.owner.pid)!=job.owner.start){if(job.armed)cleanup(&job);unlink(path);continue;}
            if(!job.armed)continue;
            if(worker&&job.owner.pid==active.owner.pid)continue;
            if(worker){cleanup(&job);unlink(path);syslog(LOG_WARNING,"concurrent snapshot skipped pid=%d",job.owner.pid);continue;}
            if(retry){kill(retry,SIGKILL);waitpid(retry,0,0);retry=0;}
            active=job;strcpy(active_path,path);deadline=seconds()+120;
            worker=fork();if(!worker){close(fd);close(lock);collect(&active,transport);}
            if(worker<0){worker=0;cleanup(&active);unlink(active_path);}
            else {int state;setpgid(worker,worker);active.collector.pid=worker;active.collector.start=snapshot_start(worker);
                state=open(path,O_WRONLY|O_NOFOLLOW);if(state>=0){pwrite(state,&active.collector,sizeof(active.collector),offsetof(struct snap_job,collector));close(state);}
                syslog(LOG_WARNING,"capturing pid=%d tid=%d signal=%d",job.owner.pid,job.fault_tid,job.signal_number);}
        }closedir(d);
        if(retry&&waitpid(retry,0,WNOHANG)==retry)retry=0;
        if(!worker&&!retry&&seconds()>=next_retry){
            next_retry=seconds()+30;retry=fork();
            if(!retry){close(fd);close(lock);execl(transport,transport,"--retry",(char *)0);_exit(127);}
            if(retry<0)retry=0;
        }
        poll(&p,1,100);
        if(p.revents&POLLIN){char b[64];recv(fd,b,sizeof(b),MSG_DONTWAIT);}
    }
    if(retry){kill(retry,SIGKILL);waitpid(retry,0,0);}
    if(worker){kill(-worker,SIGKILL);cleanup(&active);waitpid(worker,0,0);unlink(active_path);}
    unlink(ROOT "/notify");unlink(ROOT "/daemon.pid");return 0;
}
