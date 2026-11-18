/* Capture datagrams have a queue independent of text logs and crash uploads. */
#define CAPTURE_QUEUE 256
#define CAPTURE_DATAGRAM 1200
struct capture_datagram {
    size_t length;
    unsigned char bytes[CAPTURE_DATAGRAM];
};
static struct capture_datagram packet_queue[CAPTURE_QUEUE];
static size_t packet_head, packet_count;
static unsigned long packet_drops;
static pthread_mutex_t packet_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t packet_ready=PTHREAD_COND_INITIALIZER;

static uint32_t capture_get32(const unsigned char *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}

static void capture_put32(unsigned char *p,uint32_t value) {
    unsigned i;
    for(i=0;i<4;i++)p[i]=(unsigned char)(value>>(8*i));
}

static void packet_enqueue(const unsigned char *data,size_t length) {
    uint32_t total, offset;
    if(length<=48||length>CAPTURE_DATAGRAM||memcmp(data,"GPC1",4))return;
    total=capture_get32(data+32);
    offset=capture_get32(data+36);
    if(total>4096||offset>=total||offset%1152||length-48!=(total-offset>1152?1152:total-offset))return;
    pthread_mutex_lock(&packet_mutex);
    if(packet_count==CAPTURE_QUEUE)packet_drops++;
    else {
        struct capture_datagram *item=&packet_queue[(packet_head+packet_count)%CAPTURE_QUEUE];
        item->length=length;
        memcpy(item->bytes,data,length);
        packet_count++;
        pthread_cond_signal(&packet_ready);
    }
    pthread_mutex_unlock(&packet_mutex);
}

static int capture_block(struct ftp_session *ftp,const unsigned char *data,size_t length) {
    unsigned char block[CAPTURE_DATAGRAM+20]={0};
    size_t size=16+((length+3)&~(size_t)3);
    /* Non-copyable custom block. PEN zero is reserved for local experiments. */
    capture_put32(block,0x40000bad);
    capture_put32(block+4,(uint32_t)size);
    memcpy(block+12,data,length);
    capture_put32(block+size-4,(uint32_t)size);
    return ftp_write(ftp,block,size);
}

static int capture_packet(struct ftp_session *ftp,const struct capture_datagram *item,size_t *written) {
    unsigned char block[164]={0};
    const unsigned char *data=item->bytes;
    size_t metadata, length, size;
    if(data[4]!=1||(data[5]!=10&&data[5]!=11)||capture_get32(data+36)!=0)return 0;
    metadata=data[5]==10?13:1;
    if(item->length<=48+metadata||item->length>48+metadata+127)return 0;
    length=item->length-48-metadata;
    size=32+((length+3)&~(size_t)3);
    capture_put32(block,6); /* Enhanced Packet Block */
    capture_put32(block+4,(uint32_t)size);
    capture_put32(block+8,data[5]==10?0:1);
    capture_put32(block+12,capture_get32(data+28));
    capture_put32(block+16,capture_get32(data+24));
    capture_put32(block+20,(uint32_t)length);
    capture_put32(block+24,(uint32_t)length);
    memcpy(block+28,data+48+metadata,length);
    capture_put32(block+size-4,(uint32_t)size);
    if(ftp_write(ftp,block,size))return -1;
    *written+=size;
    return 0;
}

static void *packet_upload(void *unused) {
    static const unsigned char section[]={
        0x0a,0x0d,0x0d,0x0a,28,0,0,0,0x4d,0x3c,0x2b,0x1a,
        1,0,0,0,255,255,255,255,255,255,255,255,28,0,0,0,
        /* Default timestamp resolution is microseconds. */
        1,0,0,0,20,0,0,0,195,0,0,0,127,0,0,0,20,0,0,0,
        1,0,0,0,20,0,0,0,195,0,0,0,127,0,0,0,20,0,0,0
    };
    struct ftp_session ftp;
    struct capture_datagram item;
    unsigned long reported=0;
    size_t bytes=0;
    time_t opened=0;
    char id[80],temporary[160],final[160];
    int active=0;
    (void)unused;
    item.length=0;
    ftp.control=ftp.data=-1;
    lower_upload_priority();
    while(1) {
        unsigned long lost;
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME,&deadline);
        deadline.tv_sec++;
        pthread_mutex_lock(&packet_mutex);
        while(!item.length&&!packet_count&&packet_drops==reported&&!stopping)
            if(pthread_cond_timedwait(&packet_ready,&packet_mutex,&deadline)==ETIMEDOUT)break;
        if(!item.length&&packet_count) {
            item=packet_queue[packet_head];
            packet_head=(packet_head+1)%CAPTURE_QUEUE;
            packet_count--;
        }
        lost=packet_drops;
        pthread_mutex_unlock(&packet_mutex);
        if(active&&(bytes+item.length+196>cfg.file_bytes||time(0)-opened>=(time_t)cfg.rotate_seconds||
                    (stopping&&!item.length&&lost==reported))) {
            if(!ftp_finish_upload(&ftp)&&!ftp_rename(&ftp,temporary,final))trim_logs(&ftp,1);
            else reported=0;
            ftp_close(&ftp);
            active=0;
        }
        if(!item.length&&lost==reported) {
            if(stopping)break;
            continue;
        }
        if(!active) {
            identifier(id,sizeof(id));
            snprintf(temporary,sizeof(temporary),"ftp-log-collector-capture-%s.pcapng.partial",id);
            snprintf(final,sizeof(final),"ftp-log-collector-capture-%s.pcapng",id);
            if(ftp_open(&ftp)||ftp_start_upload(&ftp,temporary)||ftp_write(&ftp,section,sizeof(section)))goto failed;
            active=1;
            opened=time(0);
            bytes=sizeof(section);
        }
        if(lost!=reported) {
            unsigned char report[12]="GPR1";
            capture_put32(report+4,(uint32_t)lost);
            if(capture_block(&ftp,report,sizeof(report)))goto failed;
            bytes+=sizeof(report)+16;
            reported=lost;
        }
        if(item.length) {
            if(capture_block(&ftp,item.bytes,item.length)||capture_packet(&ftp,&item,&bytes))goto failed;
            bytes+=((item.length+3)&~(size_t)3)+16;
            item.length=0;
        }
        continue;
failed:
        ftp_close(&ftp);
        active=0;
        reported=0;
        if(stopping)break;
        sleep(2);
    }
    return 0;
}
