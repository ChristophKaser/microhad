/* Small Linux core_pattern pipe handler. Wire format is documented in README.md. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#define CAP (1024*1024)
#define CHUNK 16384
static int sock = -1, spool = -1, truncated;
static size_t saved;
static unsigned char buffer[CHUNK];
static char pending[1024], partial[1024];

static int waitfd(int fd, short events, int milliseconds) {
    struct pollfd p;
    int r;
    p.fd=fd; p.events=events; p.revents=0;
    do { r=poll(&p,1,milliseconds); } while(r<0 && errno==EINTR);
    return r>0 && (p.revents & events);
}
static int put(int fd, const void *data, size_t size, int network) {
    const char *p=data;
    while(size) {
        ssize_t n;
        if(network && !waitfd(fd,POLLOUT,10000)) return -1;
        n=write(fd,p,size);
        if(n<0 && (errno==EINTR || errno==EAGAIN)) continue;
        if(n<=0) return -1;
        p+=n; size-=(size_t)n;
    }
    return 0;
}
static void sendpart(const void *data, size_t size) {
    if(sock>=0 && put(sock,data,size,1)) { close(sock); sock=-1; }
}
static void frame(const void *data, size_t size) {
    uint32_t length=htonl((uint32_t)size);
    sendpart(&length,4); sendpart(data,size);
    /* Leave room for final zero frame and completion byte. */
    if(spool>=0 && !truncated) {
        if(saved+4+size+5>CAP) truncated=1;
        else if(put(spool,&length,4,0) || put(spool,data,size,0)) {
            close(spool); spool=-1; unlink(partial);
        } else saved+=4+size;
    }
}
static int connect_host(const char *host, int port) {
    struct sockaddr_in a;
    int fd, error=0;
    socklen_t size=sizeof(error);
    memset(&a,0,sizeof(a)); a.sin_family=AF_INET; a.sin_port=htons((unsigned short)port);
    if(inet_pton(AF_INET,host,&a.sin_addr)!=1) return -1;
    fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0) return -1;
    if(fcntl(fd,F_SETFL,O_NONBLOCK)<0) { close(fd); return -1; }
    if(connect(fd,(struct sockaddr *)&a,sizeof(a))<0 && errno!=EINPROGRESS) { close(fd); return -1; }
    if(!waitfd(fd,POLLOUT,3000) || getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size) || error) { close(fd); return -1; }
    return fd;
}
static int acknowledged(void) {
    char ack;
    if(sock<0) return 0;
    if(shutdown(sock,SHUT_WR)) return 0;
    return waitfd(sock,POLLIN,10000) && read(sock,&ack,1)==1 && ack=='K';
}
static int open_private(const char *path) {
    return open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
}
/* Recover a killed producer's bounded prefix under the transport lock. */
static void recover_partial(void) {
    int fd,valid=0,complete=0;struct stat st;char header[128],status='T';
    size_t used=0;off_t end=0;uint32_t size,zero=0;
    if(!lstat(pending,&st))return;
    fd=open(partial,O_RDWR|O_NOFOLLOW);if(fd<0)return;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size>CAP)goto discard;
    while(used<sizeof(header)-1){if(read(fd,header+used,1)!=1)goto discard;if(header[used++]=='\n')break;}
    header[used]=0;if(strncmp(header,"GCORE1 ",7)||!used||header[used-1]!='\n')goto discard;
    end=(off_t)used;
    for(;;){
        off_t at=lseek(fd,0,SEEK_CUR);ssize_t n=read(fd,&size,4);
        if(n!=4)break;
        size=ntohl(size);
        if(!size){if(valid&&read(fd,&status,1)==1&&(status=='C'||status=='T')&&at+5==st.st_size){end=st.st_size;complete=1;}break;}
        if(size>CHUNK||at+4+(off_t)size>st.st_size)break;
        if(lseek(fd,size,SEEK_CUR)<0)break;
        end=at+4+(off_t)size;valid=1;
    }
    if(!valid||end+(!complete?5:0)>CAP)goto discard;
    if(!complete){
        if(ftruncate(fd,end)||lseek(fd,end,SEEK_SET)<0||put(fd,&zero,4,0)||put(fd,"T",1,0))goto discard;
    }
    close(fd);if(rename(partial,pending))unlink(partial);return;
discard:
    close(fd);unlink(partial);
}
int main(int argc, char **argv) {
    char config[1024], root[768]="/tmp/gateway-core-capture", host[64], path[1100];
    char header[128], meta[CHUNK], exe[1024];
    int port, lock, fd, retry=0, offset=1, snapshot=0;
    uint64_t expected=0,received=0;
    ssize_t n;
    struct stat st;
    FILE *f;
    signal(SIGPIPE,SIG_IGN);
    umask(077);
    openlog("gateway-core",LOG_PID,LOG_DAEMON);
    n=readlink("/proc/self/exe",config,sizeof(config)-32);
    if(n<0) return 2;
    config[n]=0;
    strcpy(strrchr(config,'/')+1,"core-capture.conf");
    /* Explicit local/test overrides, never sourced as shell code. */
    if(argc>=5 && !strcmp(argv[1],"--paths")) {
        if(strlen(argv[2])>=sizeof(config) || strlen(argv[3])>=sizeof(root)) return 2;
        strcpy(config,argv[2]); strcpy(root,argv[3]); offset=4;
    }
    if(argc==offset+4 && !strcmp(argv[offset],"--snapshot")){snapshot=1;offset++;}
    if(argc==offset+1 && !strcmp(argv[offset],"--retry")) retry=1;
    else if(argc!=offset+3) return 2;
    if(!retry) { int i; for(i=offset;i<argc;i++)
        if(!argv[i][0] || strspn(argv[i],"0123456789")!=strlen(argv[i])) return 2; }
    f=fopen(config,"r");
    if(!f) return 2;
    n=fscanf(f,"%63s %d",host,&port); fclose(f);
    if(n!=2 || port<1 || port>65535 || strncmp(root,"/tmp/",5)) return 2;
    if(mkdir(root,0700) && errno!=EEXIST) return 2;
    if(lstat(root,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077)) return 2;
    snprintf(path,sizeof(path),"%s/lock",root);
    lock=open(path,O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    if(lock<0) return 2;
    if(flock(lock,LOCK_EX|LOCK_NB)) {
        if(!retry) syslog(LOG_WARNING,"concurrent core capture skipped");
        return 1;
    }
    snprintf(pending,sizeof(pending),"%s/pending",root);
    snprintf(partial,sizeof(partial),"%s/partial",root);
    recover_partial();
    if(retry) {
        fd=open(pending,O_RDONLY|O_NOFOLLOW);
        if(fd<0)return 1;
        sock=connect_host(host,port);
        if(sock<0)return 1;
        while((n=read(fd,buffer,sizeof(buffer)))>0) sendpart(buffer,(size_t)n);
        close(fd);
        if(n<0 || !acknowledged()) return 1;
        unlink(pending); syslog(LOG_INFO,"RAM fallback delivered"); return 0;
    }
    sock=connect_host(host,port);
    /* Never overwrite an older pending capture. Partial is owned under lock. */
    unlink(partial);
    if(lstat(pending,&st) && errno==ENOENT) spool=open_private(partial);
    if(spool<0) syslog(LOG_WARNING,"RAM fallback unavailable (older capture retained or storage failure)");
    n=snprintf(header,sizeof(header),"GCORE1 %s %s %s\n",argv[offset],argv[offset+1],argv[offset+2]);
    if(n<=0 || (size_t)n>=sizeof(header)) return 2;
    sendpart(header,(size_t)n);
    if(spool>=0) { if(put(spool,header,(size_t)n,0)) {close(spool); spool=-1; unlink(partial);} else saved=(size_t)n; }
    snprintf(path,sizeof(path),"/proc/%s/exe",argv[offset]);
    n=readlink(path,exe,sizeof(exe)-1); if(n<0) n=0; exe[n]=0;
    n=snprintf(meta,sizeof(meta),"exe=%s\nmaps:\n",exe);
    snprintf(path,sizeof(path),"/proc/%s/maps",argv[offset]);
    fd=open(path,O_RDONLY);
    if(fd>=0) { ssize_t r=read(fd,meta+n,sizeof(meta)-(size_t)n); if(r>0) n+=r; close(fd); }
    frame(meta,(size_t)n);
    if(snapshot){unsigned char length[8];int i;size_t got=0;
        while(got<8){ssize_t r;if(!waitfd(0,POLLIN,120000))break;r=read(0,length+got,8-got);if(r<=0)break;got+=r;}
        if(got!=8){if(spool>=0)close(spool);unlink(partial);return 1;}
        for(i=0;i<8;i++)expected=(expected<<8)|length[i];
    }
    for(;;) {
        struct pollfd input;
        int ready;
        input.fd=STDIN_FILENO; input.events=POLLIN; input.revents=0;
        do { ready=poll(&input,1,snapshot?120000:10000); } while(ready<0 && errno==EINTR);
        if(ready<=0) { n=-1; break; }
        n=read(STDIN_FILENO,buffer,sizeof(buffer));
        if(n<0 && errno==EINTR) continue;
        if(n<=0) break;
        received+=(size_t)n;
        frame(buffer,(size_t)n);
    }
    { uint32_t zero=0; char complete=n==0&&(!snapshot||received==expected)?'C':'T';
      sendpart(&zero,4); sendpart(&complete,1);
      if(spool>=0) {
          char status=truncated || n<0 || (snapshot&&received!=expected)?'T':'C';
          if(put(spool,&zero,4,0) || put(spool,&status,1,0)) {close(spool);spool=-1;unlink(partial);}
          else {close(spool);spool=-1; if(rename(partial,pending)) unlink(partial);}
      }
    }
    if(acknowledged()) {
        /* Only remove the fallback belonging to this capture. */
        if(saved) unlink(pending);
        syslog(LOG_INFO,"core streamed and saved by host"); return 0;
    }
    syslog(LOG_WARNING,"core transfer failed; RAM fallback %lu bytes%s",(unsigned long)saved,truncated?" (truncated)":"");
    return 1;
}
