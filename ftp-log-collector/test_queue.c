/* Exercise real queue configuration and overflow reporting without a device. */
#define syslog recorded_syslog
#define main collector_program_main
#include "main.c"
#undef main
#undef syslog
#include <assert.h>
#include <stdarg.h>

static unsigned reports;
static unsigned long last_count;
void recorded_syslog(int priority, const char *format, ...) {
    va_list args;
    (void)priority;(void)format;
    va_start(args,format);last_count=va_arg(args,unsigned long);va_end(args);
    reports++;
}
int main(int argc,char **argv) {
    struct timespec last={0,0},now={100,500000000};
    unsigned long reported=0;
    FILE *config;
    assert(argc==2 && read_config(argv[1])==0);
    assert(cfg.queue_bytes==512*1024);
    config=fopen(argv[1],"a");assert(config);
    assert(fputs("queue_bytes=4096\n",config)>=0);assert(fclose(config)==0);
    assert(read_config(argv[1])==0 && cfg.queue_bytes==4096);
    queue=malloc(cfg.queue_bytes);assert(queue);
    enqueue((const unsigned char *)"first",5);
    used=cfg.queue_bytes; /* A stalled FTP connection has filled the queue. */
    enqueue((const unsigned char *)"lost",4);
    report_drops(&last,&reported,&now);
    assert(reports==1 && last_count==1);
    for(unsigned i=0;i<1000;i++) {
        enqueue((const unsigned char *)"lost",4);
        report_drops(&last,&reported,&now);
    }
    assert(reports==1);
    now.tv_sec=101;now.tv_nsec=499999999;
    report_drops(&last,&reported,&now);assert(reports==1);
    now.tv_nsec=500000000;
    report_drops(&last,&reported,&now);
    assert(reports==2 && last_count==1001);
    now.tv_sec=102;report_drops(&last,&reported,&now);assert(reports==2);
    enqueue((const unsigned char *)"lost",4);
    report_drops(&last,&reported,&now);assert(reports==3 && last_count==1002);
    free(queue);
    puts("Queue default/override and burst-drop throttling: passed");
    return 0;
}
