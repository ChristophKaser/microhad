/* Bounded maintenance only: ownership checks, Spinel bootloader entry, Unix COBS probe.
 * No firmware upload, network formation or token/key writes. */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <termios.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void expired(int sig) { (void)sig; _exit(124); }
static int allowed(const char *path) {
    if (!strcmp(path,"/dev/ttyS1")) return 1;
#ifdef HOST_TEST
    if (!strncmp(path,"/dev/pts/",9)) return 1;
#endif
    return 0;
}
/* Fail closed if /proc inspection is incomplete. Never trust a process name. */
static int owners(const char *path, pid_t expected) {
    struct stat target, entry;
    DIR *proc; struct dirent *pid;
    int found=0, fail=0;
    if (stat(path,&target) || !S_ISCHR(target.st_mode)) return -1;
#ifndef HOST_TEST
    if (geteuid()!=0) return -1;
    if (expected>0) {
        char command[128], executable[128];int command_fd;ssize_t length;
        snprintf(command,sizeof(command),"/proc/%ld/cmdline",(long)expected);
        command_fd=open(command,O_RDONLY);
        if(command_fd<0) return -1;
        length=read(command_fd,executable,sizeof(executable)-1);close(command_fd);
        if(length<=0) return -1;
        executable[length]=0;
        if(strcmp(executable,"/tuya/serialgateway")) return -1;
    }
#endif
    proc=opendir("/proc"); if (!proc) return -1;
    while ((pid=readdir(proc))) {
        char *end; long number=strtol(pid->d_name,&end,10);
        char directory[128]; DIR *fds; struct dirent *item;
        if (*end || number<=0 || number==(long)getpid()) continue;
        snprintf(directory,sizeof(directory),"/proc/%ld/fd",number);
        fds=opendir(directory);
        if (!fds) {
#ifdef HOST_TEST
            /* Unprivileged host tests cannot inspect root-owned processes; the
             * production root helper refuses these incomplete inspections. */
            if (errno==EACCES) continue;
#endif
            if (errno!=ENOENT && errno!=ESRCH) fail=1;
            continue;
        }
        while ((item=readdir(fds))) {
            char file[256];
            if (item->d_name[0]=='.') continue;
            if (snprintf(file,sizeof(file),"%s/%s",directory,item->d_name)>=(int)sizeof(file)) { fail=1;break; }
            if (stat(file,&entry)) {
#ifdef HOST_TEST
                if (errno==EACCES) continue;
#endif
                if (errno!=ENOENT && errno!=ESRCH) fail=1;
                continue;
            }
            if (S_ISCHR(entry.st_mode) && entry.st_rdev==target.st_rdev) {
                if (number!=(long)expected) fail=1;
                found=1;
            }
        }
        closedir(fds);
    }
    closedir(proc);
    return fail || (expected>0 && !found) ? -1 : 0;
}
static int setup(int fd, speed_t speed, int flow) {
    struct termios term;
    if (tcgetattr(fd,&term)) return -1;
    cfmakeraw(&term); term.c_cflag|=CLOCAL|CREAD;
    term.c_cflag&=~CRTSCTS; if (flow) term.c_cflag|=CRTSCTS;
    term.c_cc[VMIN]=0; term.c_cc[VTIME]=5;
    if (cfsetispeed(&term,speed) || cfsetospeed(&term,speed)) return -1;
    return tcsetattr(fd,TCSANOW,&term);
}
static int write_all(int fd,const unsigned char *data,size_t count) {
    size_t at=0;
    while (at<count) {
        ssize_t n=write(fd,data+at,count-at);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return -1;
        at+=(size_t)n;
    }
    return 0;
}
static size_t reset_frame(unsigned char *out) {
    /* Multi-PAN IID0 is reserved and has no instance. RESET must address
     * the active Zigbee instance, IID1, even for a whole-chip bootloader reset. */
    const unsigned char input[]={0x90,0x01,0x03}; /* IID1, RESET, BOOTLOADER */
    unsigned short fcs=0xffff; unsigned char body[5]; size_t i,at=0;
    memcpy(body,input,3);
    for(i=0;i<3;i++) {
        int bit; fcs^=body[i];
        for(bit=0;bit<8;bit++) fcs=(fcs>>1)^((fcs&1)?0x8408:0);
    }
    fcs^=0xffff;body[3]=fcs&255;body[4]=fcs>>8;out[at++]=0x7e;
    for(i=0;i<5;i++) {
        unsigned char c=body[i];
        if(c==0x7e||c==0x7d||c==0x11||c==0x13||c==0xf8) {out[at++]=0x7d;out[at++]=c^0x20;}
        else out[at++]=c;
    }
    out[at++]=0x7e;return at;
}
static int serial_open(const char *path) {
    int fd;
    if (owners(path,0)) return -1;
    fd=open(path,O_RDWR|O_NOCTTY|O_NONBLOCK);
    if (fd<0) return -1;
    if (ioctl(fd,TIOCEXCL) || owners(path,0)) {close(fd);return -1;}
    if (fcntl(fd,F_SETFL,0)<0) {close(fd);return -1;}
    return fd;
}
static void serial_close(int fd) { ioctl(fd,TIOCNXCL);close(fd); }
static int menu(int fd, int run) {
    char data[4097];size_t used=0;
    time_t end=time(NULL)+10;
    const unsigned char newline='\n';
    if (write_all(fd,&newline,1)) return -1;
    while (used<sizeof(data)-1 && time(NULL)<end) {
        ssize_t n=read(fd,data+used,sizeof(data)-1-used);
        if(n<0 && errno==EINTR) continue;
        if(n<0) return -1;
        used+=(size_t)n;data[used]=0;
        if(strstr(data,"Gecko Bootloader") && strstr(data,"2. run") && strstr(data,"BL >")) {
            if(run && write_all(fd,(const unsigned char *)"2\n",2)) return -1;
            puts(run?"Bootloader menu verified; run requested":"Bootloader menu verified; no upload/run command sent");return 0;
        }
    }
    return -1;
}
static int unix_open(const char *path) {
    struct sockaddr_un addr; int fd;
    struct stat state;
    if (strcmp(path,"/tmp/ziggurat/native.sock")) {
#ifdef HOST_TEST
        if(strncmp(path,"/tmp/",5)) return -1;
#else
        return -1;
#endif
    }
    if(lstat(path,&state) || !S_ISSOCK(state.st_mode) || state.st_uid!=geteuid() || (state.st_mode&077)) return -1;
    memset(&addr,0,sizeof(addr));addr.sun_family=AF_UNIX;
    if(strlen(path)>=sizeof(addr.sun_path)) return -1;
    strcpy(addr.sun_path,path);fd=socket(AF_UNIX,SOCK_STREAM,0);
    if(fd<0) return -1;
    if(connect(fd,(struct sockaddr *)&addr,sizeof(addr))) {close(fd);return -1;}
    return fd;
}
static int bridge(int fd) {
    for(;;) {
        fd_set reads;int r;int i;
        FD_ZERO(&reads);FD_SET(0,&reads);FD_SET(fd,&reads);
        r=select(fd+1,&reads,NULL,NULL,NULL);
        if(r<0 && errno==EINTR) continue;
        if(r<0) return 1;
        for(i=0;i<2;i++) {
            int from=i?fd:0,to=i?1:fd;
            if(FD_ISSET(from,&reads)) {
                unsigned char buffer[2048];ssize_t n=read(from,buffer,sizeof(buffer));
                if(n==0) return 0;
                if(n<0 || write_all(to,buffer,(size_t)n)) return 1;
            }
        }
    }
}
int main(int argc,char **argv) {
    int fd,result;
    signal(SIGALRM,expired);alarm(25);
    if(argc==3 && (!strcmp(argv[1],"cobs-hello") || !strcmp(argv[1],"cobs-bridge"))) {
        fd=unix_open(argv[2]);if(fd<0) return 1;
        if(!strcmp(argv[1],"cobs-bridge")) {alarm(120);result=bridge(fd);}
        else {
            /* exact protocol-v2 initial Hello notification, both worker-ready states */
            unsigned char buffer[8];size_t at=0;alarm(5);
            while(at<7) {ssize_t n=read(fd,buffer+at,7-at);if(n<=0) break;at+=(size_t)n;}
            result=1;
            if(at==7 && !memcmp(buffer,"\x01\x02\x03\x02\x02\x01\x00",7)) result=0;
            if(at==7 && !memcmp(buffer,"\x01\x02\x03\x03\x02\x01\x00",7)) result=0;
        }
        close(fd);return result;
    }
    if(argc==3 && !strcmp(argv[1],"check-free") && allowed(argv[2])) return owners(argv[2],0)?1:0;
    if(argc==4 && !strcmp(argv[1],"check-owner") && allowed(argv[2])) {
        char *end;long expected=strtol(argv[3],&end,10);
        if(*end || expected<=1) return 2;
        return owners(argv[2],(pid_t)expected)?1:0;
    }
    if(argc!=4 || !allowed(argv[2]) || strcmp(argv[3],"--exclusive-radio") ||
       (strcmp(argv[1],"bootloader-enter") && strcmp(argv[1],"bootloader-run"))) return 2;
    fd=serial_open(argv[2]);if(fd<0) {fputs("UART ownership/exclusivity check failed\n",stderr);return 1;}
    if(!strcmp(argv[1],"bootloader-enter")) {
        unsigned char frame[16];size_t n=reset_frame(frame);
        if(setup(fd,B460800,1) || tcflush(fd,TCIOFLUSH) || write_all(fd,frame,n)) {serial_close(fd);return 1;}
        /* tcdrain may block on RTS/CTS; the global alarm bounds it. */
        if(tcdrain(fd)) {serial_close(fd);return 1;}
        serial_close(fd);sleep(3);
        fd=serial_open(argv[2]);if(fd<0) return 1;
    }
    result=setup(fd,B115200,0)?-1:menu(fd,!strcmp(argv[1],"bootloader-run"));
    serial_close(fd);return result?1:0;
}
