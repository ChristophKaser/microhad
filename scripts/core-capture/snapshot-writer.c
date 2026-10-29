#define _GNU_SOURCE
#include "snapshot.h"
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/procfs.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

unsigned long snapshot_start(int pid) {
    char path[64],line[1024],*p;int field;unsigned long start=0;FILE *f;
    snprintf(path,sizeof(path),"/proc/%d/stat",pid);f=fopen(path,"r");
    if(!f)return 0;
    if(fgets(line,sizeof(line),f)) {
        p=strrchr(line,')');
        if(p) {p+=2;for(field=3;field<22&&*p;field++){p=strchr(p,' ');if(!p)break;p++;}
            if(p)start=strtoul(p,0,10);}
    }
    fclose(f);return start;
}
static int parent(int pid) {
    char path[64],line[256];FILE *f;int pp=0;
    snprintf(path,sizeof(path),"/proc/%d/status",pid);f=fopen(path,"r");if(!f)return 0;
    while(fgets(line,sizeof(line),f))if(sscanf(line,"PPid: %d",&pp)==1)break;
    fclose(f);return pp;
}
static void add(struct snap_task *tasks,int *count,int pid,unsigned long start) {
    int i;if(pid<=1||!start||snapshot_start(pid)!=start)return;
    for(i=0;i<*count;i++)if(tasks[i].pid==pid)return;
    if(*count<SNAP_MAX_THREADS+2){tasks[*count].pid=pid;tasks[*count].start=start;(*count)++;}
}
int snapshot_tasks(struct snap_job *job,struct snap_task *tasks) {
    int count=0,i;char path[64];DIR *d;struct dirent *e;
    if(job->magic!=SNAP_MAGIC||snapshot_start(job->owner.pid)!=job->owner.start)return 0;
    if(job->fault_tid==job->owner.pid)add(tasks,&count,job->owner.pid,job->owner.start);
    else for(i=0;i<SNAP_MAX_THREADS;i++)if(job->threads[i].pid==job->fault_tid)
        add(tasks,&count,job->fault_tid,job->threads[i].start);
    if(!count)return 0;
    add(tasks,&count,job->owner.pid,job->owner.start);
    for(i=0;i<SNAP_MAX_THREADS;i++) {
        int pid=job->threads[i].pid,pp;
        if(pid<=1||snapshot_start(pid)!=job->threads[i].start)continue;
        add(tasks,&count,pid,job->threads[i].start);
        pp=parent(pid);
        /* LinuxThreads worker parent is the manager, itself a child of main. */
        if(pp!=job->owner.pid&&parent(pp)==job->owner.pid)
            add(tasks,&count,pp,snapshot_start(pp));
    }
    snprintf(path,sizeof(path),"/proc/%d/task",job->owner.pid);d=opendir(path);
    if(d){while((e=readdir(d))) {int pid=atoi(e->d_name);if(pid>1)add(tasks,&count,pid,snapshot_start(pid));}closedir(d);}
    /* LinuxThreads also has tasks whose start trampoline has not registered
     * yet. Restrict discovery to the same executable and the manager subtree;
     * these services do not fork same-executable application children. */
    {struct stat owner,candidate;char exe[64];
     snprintf(exe,sizeof(exe),"/proc/%d/exe",job->owner.pid);
     if(!stat(exe,&owner)){
      d=opendir("/proc");
      if(d){while((e=readdir(d))){int pid=atoi(e->d_name),pp;
       if(pid<=1)continue;
       pp=parent(pid);
       if(pp!=job->owner.pid&&parent(pp)!=job->owner.pid)continue;
       snprintf(exe,sizeof(exe),"/proc/%d/exe",pid);
       if(!stat(exe,&candidate)&&candidate.st_dev==owner.st_dev&&candidate.st_ino==owner.st_ino)
        add(tasks,&count,pid,snapshot_start(pid));
      }closedir(d);}
     }}
    return count;
}
void snapshot_kill(struct snap_task *tasks,int count) {
    int i;
    for(i=0;i<count;i++)if(snapshot_start(tasks[i].pid)==tasks[i].start)kill(tasks[i].pid,SIGKILL);
}
static int put(const void *buffer,size_t size) {
    const char *p=buffer;
    while(size){ssize_t n=write(STDOUT_FILENO,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;size-=n;}
    return 0;
}
int snapshot_write(struct snap_job *job) {
    struct snap_task tasks[SNAP_MAX_THREADS+2];
    int count=snapshot_tasks(job,tasks),i,j,fd=-1,status,result=-1,maps=0;
    unsigned char notes[(SNAP_MAX_THREADS+2)*512],data[16384];size_t used=0,position;
    Elf32_Ehdr eh;Elf32_Phdr ph[513];FILE *f;char path[64],line[512];
    if(!count)return -1;
    memset(ph,0,sizeof(ph));
    for(i=0;i<count;i++) {
        uint64_t regs[38];struct elf_prstatus pr;Elf32_Nhdr nh;
        if(ptrace(PTRACE_ATTACH,tasks[i].pid,0,0)<0)return -1;
        if(waitpid(tasks[i].pid,&status,__WALL)<0||!WIFSTOPPED(status))return -1;
        memset(regs,0,sizeof(regs));
        if(ptrace(PTRACE_GETREGS,tasks[i].pid,0,regs)<0)return -1;
        if(tasks[i].pid==job->fault_tid)memcpy(regs,job->regs,sizeof(regs));
        memset(&pr,0,sizeof(pr));pr.pr_pid=tasks[i].pid;pr.pr_ppid=job->owner.pid;
        pr.pr_cursig=job->signal_number;pr.pr_info.si_signo=job->signal_number;pr.pr_info.si_code=job->signal_code;
        for(j=0;j<32;j++)pr.pr_reg[6+j]=(unsigned long)regs[j];
        pr.pr_reg[38]=regs[32];pr.pr_reg[39]=regs[33];pr.pr_reg[40]=regs[34];pr.pr_reg[41]=job->fault_address;
        nh.n_namesz=5;nh.n_descsz=sizeof(pr);nh.n_type=NT_PRSTATUS;
        memcpy(notes+used,&nh,sizeof(nh));used+=sizeof(nh);memcpy(notes+used,"CORE\0\0\0\0",8);used+=8;
        memcpy(notes+used,&pr,sizeof(pr));used+=sizeof(pr);
        if(i==count-1){struct snap_task fresh[SNAP_MAX_THREADS+2];int n=snapshot_tasks(job,fresh),k;
            for(k=0;k<n;k++)add(tasks,&count,fresh[k].pid,fresh[k].start);
        }
    }
    snprintf(path,sizeof(path),"/proc/%d/maps",job->owner.pid);f=fopen(path,"r");if(!f)return -1;
    while(fgets(line,sizeof(line),f)) {unsigned long a,b;char mode[5];
        if(sscanf(line,"%lx-%lx %4s",&a,&b,mode)!=3||mode[0]!='r')continue;
        if(maps==512){fclose(f);return -1;}
        maps++;ph[maps].p_type=PT_LOAD;ph[maps].p_vaddr=a;ph[maps].p_memsz=b-a;
        ph[maps].p_filesz=mode[1]=='w'?b-a:0;
        ph[maps].p_flags=PF_R|(mode[1]=='w'?PF_W:0)|(mode[2]=='x'?PF_X:0);ph[maps].p_align=1;
    }fclose(f);
    memset(&eh,0,sizeof(eh));memcpy(eh.e_ident,ELFMAG,SELFMAG);eh.e_ident[EI_CLASS]=ELFCLASS32;eh.e_ident[EI_DATA]=ELFDATA2MSB;eh.e_ident[EI_VERSION]=EV_CURRENT;
    eh.e_type=ET_CORE;eh.e_machine=EM_MIPS;eh.e_version=EV_CURRENT;eh.e_ehsize=sizeof(eh);eh.e_phoff=sizeof(eh);eh.e_phentsize=sizeof(ph[0]);eh.e_phnum=maps+1;eh.e_flags=0x1000;
    position=sizeof(eh)+(maps+1)*sizeof(ph[0]);ph[0].p_type=PT_NOTE;ph[0].p_offset=position;ph[0].p_filesz=used;position+=used;
    for(i=1;i<=maps;i++){ph[i].p_offset=position;position+=ph[i].p_filesz;if(position>0x7fffffff)return -1;}
    {unsigned char length[8];int k;for(k=0;k<8;k++)length[k]=(uint64_t)position>>((7-k)*8);
     if(put(length,8))return -1;}
    if(put(&eh,sizeof(eh))||put(ph,(maps+1)*sizeof(ph[0]))||put(notes,used))return -1;
    snprintf(path,sizeof(path),"/proc/%d/mem",job->owner.pid);fd=open(path,O_RDONLY);if(fd<0)return -1;
    for(i=1;i<=maps;i++){unsigned long offset=0;
        while(offset<ph[i].p_filesz){size_t n=ph[i].p_filesz-offset;if(n>sizeof(data))n=sizeof(data);
            if(lseek(fd,ph[i].p_vaddr+offset,SEEK_SET)<0||read(fd,data,n)!=(ssize_t)n)goto done;
            if(put(data,n))goto done;
            offset+=n;
        }
    }
    result=0;
done:
    close(fd);return result;
}
