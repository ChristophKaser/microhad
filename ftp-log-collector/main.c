#define _GNU_SOURCE
#include "ftp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define MAX_FRAME 16384
#define MAX_PACKET 2048
#define MAX_NAMES 10000
struct config {
    char host[256], user[256], password[256], directory[512];
    unsigned ftp_port, udp_port, tcp_port, keep, rotate_seconds;
    size_t queue_bytes, file_bytes;
};
static struct config cfg;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static unsigned char *queue;
static size_t head, tail, used;
static unsigned long dropped;
static volatile sig_atomic_t stopping;
static unsigned serial;
static pthread_mutex_t serial_mutex = PTHREAD_MUTEX_INITIALIZER;

static void stop(int sig) { (void)sig; stopping = 1; }
static int number(const char *s, unsigned long min, unsigned long max, unsigned long *out) {
    char *end; unsigned long n;
    if (!*s || *s == '-') return -1;
    errno = 0; n = strtoul(s, &end, 10);
    if (errno || *end || n < min || n > max) return -1;
    *out = n; return 0;
}
static int copy_field(char *out, size_t cap, const char *value) {
    size_t n = strlen(value); if (!n || n >= cap || strchr(value, '\r')) return -1;
    memcpy(out,value,n+1); return 0;
}
static int read_config(const char *path) {
    FILE *f; char line[1024]; struct stat st; unsigned seen=0;
    memset(&cfg,0,sizeof(cfg)); cfg.ftp_port=21; cfg.udp_port=5514; cfg.tcp_port=9999;
    cfg.keep=100; cfg.rotate_seconds=3600; cfg.queue_bytes=128*1024; cfg.file_bytes=1024*1024;
    f=fopen(path,"r"); if (!f) return -1;
    if (fstat(fileno(f),&st) || !S_ISREG(st.st_mode) || (st.st_mode & 077) || st.st_uid != geteuid()) { fclose(f); return -1; }
    while (fgets(line,sizeof(line),f)) {
        char *eq,*end; unsigned long n;
        end=strchr(line,'\n'); if (!end && !feof(f)) goto fail; if(end)*end=0;
        if (!line[0] || line[0]=='#') continue;
        eq=strchr(line,'='); if(!eq) goto fail; *eq++=0;
        if(!strcmp(line,"ftp_host")) { if(copy_field(cfg.host,sizeof(cfg.host),eq))goto fail;seen|=1; }
        else if(!strcmp(line,"ftp_user")) { if(copy_field(cfg.user,sizeof(cfg.user),eq))goto fail;seen|=2; }
        else if(!strcmp(line,"ftp_password")) { if(copy_field(cfg.password,sizeof(cfg.password),eq))goto fail;seen|=4; }
        else if(!strcmp(line,"ftp_directory")) { if(copy_field(cfg.directory,sizeof(cfg.directory),eq))goto fail;seen|=8; }
        else if(!strcmp(line,"ftp_port")) { if(number(eq,1,65535,&n))goto fail;cfg.ftp_port=(unsigned)n; }
        else if(!strcmp(line,"udp_port")) { if(number(eq,1,65535,&n))goto fail;cfg.udp_port=(unsigned)n; }
        else if(!strcmp(line,"tcp_port")) { if(number(eq,1,65535,&n))goto fail;cfg.tcp_port=(unsigned)n; }
        else if(!strcmp(line,"log_file_bytes")) { if(number(eq,1024,1073741824UL,&n))goto fail;cfg.file_bytes=(size_t)n; }
        else if(!strcmp(line,"log_files")) { if(number(eq,1,10000,&n))goto fail;cfg.keep=(unsigned)n; }
        else if(!strcmp(line,"queue_bytes")) { if(number(eq,4096,4194304,&n))goto fail;cfg.queue_bytes=(size_t)n; }
        else if(!strcmp(line,"rotate_seconds")) { if(number(eq,1,86400,&n))goto fail;cfg.rotate_seconds=(unsigned)n; }
        else goto fail;
    }
    if(ferror(f)||seen!=15)goto fail;
    fclose(f); return 0;
fail: fclose(f); return -1;
}
static int listener(int type,unsigned port) {
    int fd=socket(AF_INET,type,0), one=1; struct sockaddr_in a;
    if(fd<0)return -1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons((unsigned short)port);
    if(bind(fd,(struct sockaddr*)&a,sizeof(a)) || (type==SOCK_STREAM && listen(fd,4))) {close(fd);return -1;}
    return fd;
}
static void enqueue(const unsigned char *p,size_t n) {
    size_t i; if(n>MAX_PACKET)return;
    pthread_mutex_lock(&mutex);
    if(cfg.queue_bytes-used<n+2) dropped++;
    else {
        queue[tail++]=(unsigned char)(n>>8); if(tail==cfg.queue_bytes)tail=0;
        queue[tail++]=(unsigned char)n; if(tail==cfg.queue_bytes)tail=0;
        for(i=0;i<n;i++){queue[tail++]=p[i];if(tail==cfg.queue_bytes)tail=0;}
        used+=n+2;pthread_cond_signal(&ready);
    }
    pthread_mutex_unlock(&mutex);
}
static size_t dequeue(unsigned char *p,unsigned seconds) {
    size_t n=0,i; struct timespec deadline;
    clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=seconds;
    pthread_mutex_lock(&mutex);
    while(!used&&!stopping) if(pthread_cond_timedwait(&ready,&mutex,&deadline)==ETIMEDOUT)break;
    if(used){n=(size_t)queue[head++]<<8;if(head==cfg.queue_bytes)head=0;n|=queue[head++];if(head==cfg.queue_bytes)head=0;
        for(i=0;i<n;i++){p[i]=queue[head++];if(head==cfg.queue_bytes)head=0;}used-=n+2;}
    pthread_mutex_unlock(&mutex);return n;
}
static void identifier(char *out,size_t n) {
    struct timeval t; unsigned id;gettimeofday(&t,0);
    pthread_mutex_lock(&serial_mutex);id=++serial;pthread_mutex_unlock(&serial_mutex);
    snprintf(out,n,"%010lu-%06lu-%05lu-%08u",(unsigned long)t.tv_sec,(unsigned long)t.tv_usec,(unsigned long)getpid(),id);
}
static int ftp_open(struct ftp_session *s) { return ftp_connect(s,cfg.host,cfg.ftp_port,cfg.user,cfg.password,cfg.directory); }
struct names { char **v; size_t count; };
static int collect_name(const char *name,void *context) {
    struct names *n=context; size_t i,l=strlen(name),suffix; const char *prefix="ftp-log-collector-log-";
    if(l<strlen(prefix)+5 || strncmp(name,prefix,strlen(prefix)))return 0;
    if(l>=4&&!strcmp(name+l-4,".log"))suffix=4;
    else if(l>=8&&!strcmp(name+l-8,".partial"))suffix=8;
    else return 0;
    for(i=strlen(prefix);i<l-suffix;i++)if((name[i]<'0'||name[i]>'9')&&name[i]!='-')return 0;
    if(n->count==MAX_NAMES)return -1;
    n->v[n->count]=strdup(name); if(!n->v[n->count])return -1;n->count++;return 0;
}
static int compare_names(const void *a,const void *b) { return strcmp(*(char * const *)a,*(char * const *)b); }
static void trim_logs(struct ftp_session *s) {
    struct names n;size_t i; n.count=0;n.v=calloc(MAX_NAMES,sizeof(*n.v));if(!n.v)return;
    if(!ftp_list(s,collect_name,&n)) {
        qsort(n.v,n.count,sizeof(*n.v),compare_names);
        for(i=0;i+cfg.keep<n.count;i++) if(ftp_delete(s,n.v[i]))break;
    }
    for(i=0;i<n.count;i++)free(n.v[i]);
    free(n.v);
}
static void *logs(void *unused) {
    unsigned char packet[MAX_PACKET+1]; struct ftp_session ftp;size_t bytes=0,n=0;time_t opened=0,last_error=0;char tmp[160],final[160],id[80];int active=0; (void)unused;
    ftp.control=ftp.data=-1;
    while(!stopping) {
        if(!active) {
            pthread_mutex_lock(&mutex);
            while(!used&&!n&&!stopping) pthread_cond_wait(&ready,&mutex);
            bytes=used;
            pthread_mutex_unlock(&mutex);
            if(!bytes&&!n)continue;
            if(ftp_open(&ftp)){if(time(0)-last_error>=60){syslog(LOG_WARNING,"FTP log connection failed");last_error=time(0);}sleep(2);continue;}
            identifier(id,sizeof(id));snprintf(tmp,sizeof(tmp),"ftp-log-collector-log-%s.partial",id);snprintf(final,sizeof(final),"ftp-log-collector-log-%s.log",id);
            if(ftp_start_upload(&ftp,tmp)){if(time(0)-last_error>=60){syslog(LOG_WARNING,"FTP log upload could not start");last_error=time(0);}ftp_close(&ftp);sleep(2);continue;}
            bytes=0;opened=time(0);active=1;
        }
        if(!n){n=dequeue(packet,1);if(n)packet[n++]='\n';}
        if(n && (!bytes || bytes+n<=cfg.file_bytes)){
            if(ftp_write(&ftp,packet,n)){pthread_mutex_lock(&mutex);dropped++;pthread_mutex_unlock(&mutex);if(time(0)-last_error>=60){syslog(LOG_WARNING,"FTP log upload interrupted");last_error=time(0);}n=0;ftp_close(&ftp);active=0;continue;}
            bytes+=n;n=0;
        }
        if(bytes>=cfg.file_bytes || (n && bytes+n>cfg.file_bytes) || time(0)-opened>=(time_t)cfg.rotate_seconds || stopping) {
            if(!ftp_finish_upload(&ftp)&&!ftp_rename(&ftp,tmp,final))trim_logs(&ftp);
            else if(time(0)-last_error>=60){syslog(LOG_WARNING,"FTP log publication failed");last_error=time(0);}
            ftp_close(&ftp);active=0;
        }
    }
    if(active){if(!ftp_finish_upload(&ftp))ftp_rename(&ftp,tmp,final);ftp_close(&ftp);}return 0;
}
static int exact(int fd,void *data,size_t n) {
    unsigned char *p=data;while(n){ssize_t r=recv(fd,p,n,0);if(r<0&&errno==EINTR)continue;if(r<=0)return -1;p+=r;n-=(size_t)r;}return 0;
}
static int frame_length(int fd,uint32_t *n) { uint32_t v;if(exact(fd,&v,4))return -1;*n=ntohl(v);return *n>MAX_FRAME?-1:0; }
static void capture(int fd) {
    struct ftp_session ftp; char header[128],id[80],tmp[160],core[160],meta_tmp[160],meta_name[160],status,c;unsigned pid,sig;unsigned long epoch;size_t h=0;uint32_t len;unsigned char buf[MAX_FRAME],metadata[MAX_FRAME];uint32_t meta_len;struct timeval timeout;int saved=0;
    ftp.control=ftp.data=-1;timeout.tv_sec=130;timeout.tv_usec=0;setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    while(h<sizeof(header)-1){if(exact(fd,&c,1))return;header[h++]=c;if(c=='\n')break;}header[h]=0;
    if(!h||header[h-1]!='\n'||sscanf(header,"GCORE1 %u %u %lu%c",&pid,&sig,&epoch,&c)!=4||c!='\n')return;
    if(frame_length(fd,&meta_len)||!meta_len||exact(fd,metadata,meta_len))return;
    identifier(id,sizeof(id));snprintf(tmp,sizeof(tmp),"ftp-log-collector-core-%s.partial",id);
    snprintf(meta_tmp,sizeof(meta_tmp),"ftp-log-collector-core-%s.metadata.partial",id);
    snprintf(meta_name,sizeof(meta_name),"ftp-log-collector-core-%s.metadata",id);
    if(ftp_open(&ftp)||ftp_start_upload(&ftp,tmp))goto out;
    for(;;){if(frame_length(fd,&len))goto out;if(!len)break;if(exact(fd,buf,len)||ftp_write(&ftp,buf,len))goto out;}
    if(exact(fd,&status,1)||(status!='C'&&status!='T')||recv(fd,&c,1,0)!=0)goto out;
    if(ftp_finish_upload(&ftp))goto out;
    if(ftp_start_upload(&ftp,meta_tmp))goto out;
    { int n=snprintf(header,sizeof(header),"pid=%u\nsignal=%u\nepoch=%lu\ncomplete=%s\n",pid,sig,epoch,status=='C'?"true":"false");if(n<0||ftp_write(&ftp,header,(size_t)n)||ftp_write(&ftp,metadata,meta_len))goto out; }
    if(ftp_finish_upload(&ftp)||ftp_rename(&ftp,meta_tmp,meta_name))goto out;
    snprintf(core,sizeof(core),"ftp-log-collector-core-%s.%s",id,status=='C'?"core":"truncated");
    if(ftp_rename(&ftp,tmp,core))goto out;
    saved=1;
    send(fd,"K",1,MSG_NOSIGNAL);
out: ftp_close(&ftp);if(!saved)syslog(LOG_WARNING,"FTP crash capture failed");
}
static void *cores(void *arg) { int server=*(int*)arg;while(!stopping){int fd=accept(server,0,0);if(fd<0){if(errno==EINTR)continue;break;}capture(fd);close(fd);}return 0; }
int main(int argc,char **argv) {
    int udp,tcp;pthread_t log_thread,core_thread;unsigned char packet[MAX_PACKET];struct sigaction sa;unsigned long reported=0;
    if(argc==3&&!strcmp(argv[1],"--check-config"))return read_config(argv[2])?2:0;
    if(argc!=2||read_config(argv[1])){fprintf(stderr,"Usage: ftp-log-collector PRIVATE_CONFIG (invalid or insecure configuration)\n");return 2;}
    signal(SIGPIPE,SIG_IGN);memset(&sa,0,sizeof(sa));sa.sa_handler=stop;sigaction(SIGTERM,&sa,0);sigaction(SIGINT,&sa,0);
    openlog("ftp-log-collector",LOG_PID,LOG_DAEMON);queue=malloc(cfg.queue_bytes);if(!queue)return 1;
    udp=listener(SOCK_DGRAM,cfg.udp_port);tcp=listener(SOCK_STREAM,cfg.tcp_port);if(udp<0||tcp<0){syslog(LOG_ERR,"cannot bind diagnostic listeners");return 1;}
    if(pthread_create(&log_thread,0,logs,0)||pthread_create(&core_thread,0,cores,&tcp))return 1;
    while(!stopping) {
        struct timeval tv;fd_set set;int r;FD_ZERO(&set);FD_SET(udp,&set);tv.tv_sec=2;tv.tv_usec=0;r=select(udp+1,&set,0,0,&tv);
        if(r>0){ssize_t n=recv(udp,packet,sizeof(packet)-1,0);if(n>0){packet[n]=0;if(!strstr((char*)packet,"ftp-log-collector["))enqueue(packet,(size_t)n);}}
        pthread_mutex_lock(&mutex);if(dropped!=reported){reported=dropped;pthread_mutex_unlock(&mutex);syslog(LOG_WARNING,"UDP log queue dropped %lu packets",reported);}else pthread_mutex_unlock(&mutex);
    }
    close(udp);shutdown(tcp,SHUT_RDWR);close(tcp);pthread_cond_broadcast(&ready);pthread_join(log_thread,0);pthread_join(core_thread,0);free(queue);return 0;
}
