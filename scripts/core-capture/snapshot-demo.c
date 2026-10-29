#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
volatile unsigned long demo_heap_marker=0x12345678;
__attribute__((noinline)) static void crash_here(void){
    volatile unsigned long demo_stack_marker=0xabcdef12;
    volatile char *p=mmap(0,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    fprintf(stderr,"crashing marker=%lx\n",demo_stack_marker);*p=1;
}
static void *worker(void *arg){if(arg)crash_here();for(;;)pause();return 0;}
int main(int argc,char **argv){pthread_t t;pthread_attr_t attr;
    if(getenv("SNAPSHOT_DEMO_BYTES")){
        unsigned long bytes=strtoul(getenv("SNAPSHOT_DEMO_BYTES"),0,10);
        if(mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED)return 2;
    }
    pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,128*1024);
    pthread_create(&t,&attr,worker,(void *)(long)(argc>1&&!strcmp(argv[1],"worker")));
    usleep(100000);if(argc>1&&!strcmp(argv[1],"main"))crash_here();
    for(;;)pause();return 0;
}
