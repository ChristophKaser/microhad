// Synthetic host-only syslog ring. Never attaches to the production syslog key.
#include "syslog_log_reader.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdint.h>
#include <string>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <unistd.h>
namespace {
union SemArgument { unsigned short* values; };
struct Fixture {
    long key;int segment,semaphore;char* bytes;
    Fixture():key(0),segment(-1),semaphore(-1),bytes(0){
        const int32_t capacity=512*1024,cursor=0;
        for(int i=0;i<100;++i){key=0x2a000000L+getpid()*100+i;segment=shmget(key,capacity+9,IPC_CREAT|IPC_EXCL|0600);if(segment>=0)break;}
        assert(segment>=0);semaphore=semget(key,2,IPC_CREAT|IPC_EXCL|0600);assert(semaphore>=0);
        unsigned short values[2]={0,0};SemArgument arg;arg.values=values;assert(semctl(semaphore,0,SETALL,arg)==0);
        bytes=static_cast<char*>(shmat(segment,0,0));assert(bytes!=reinterpret_cast<char*>(-1));
        memset(bytes,0,capacity+9);memcpy(bytes,&capacity,4);memcpy(bytes+4,&cursor,4);
        std::string ring(1,'\0');
        while(ring.size()+64<static_cast<unsigned>(capacity)){ring+="Jan  1 00:00:00 host synthetic gateway diagnostic message";ring.push_back(0);}
        memcpy(bytes+8,ring.data(),ring.size());
    }
    ~Fixture(){shmdt(bytes);shmctl(segment,IPC_RMID,0);semctl(semaphore,0,IPC_RMID);}
};
}
int main(){
    setenv("TZ","UTC0",1);tzset();Fixture fixture;
    for(int changed=0;changed<2;++changed){
        std::string output;SyslogReadStats stats;std::clock_t start=std::clock();
        for(int i=0;i<1000;++i){
            if(changed)fixture.bytes[8+40]=static_cast<char>('a'+i%2);
            assert(SyslogLogReader::readRequestForTest(fixture.key,true,1700000000,0,0,1700000000,&output,&stats));
            assert(output=="{\"lines\":[]}");
        }
        std::printf("logs_%s_cpu_us %.6f\n",changed?"changed":"unchanged",1e6*double(std::clock()-start)/CLOCKS_PER_SEC/1000);
    }
}
