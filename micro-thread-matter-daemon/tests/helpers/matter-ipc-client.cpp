// Test-only IPC client using the same bounded native CBOR codecs as MicroHAD.
#include "matter_protocol.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static bool transfer(int fd, unsigned char* bytes, unsigned int size, bool writing) {
    for(unsigned int at=0;at<size;) {
        ssize_t n=writing?write(fd,bytes+at,size-at):read(fd,bytes+at,size-at);
        if(n<=0)return false;
        at+=static_cast<unsigned int>(n);
    }
    return true;
}
int main(int argc,char** argv) {
    sockaddr_un address;std::memset(&address,0,sizeof(address));
    if(argc!=4||std::strlen(argv[1])>=sizeof(address.sun_path))return 2;
    unsigned long opcode=std::strtoul(argv[2],0,10);
    if(opcode<1||opcode>11)return 2;
    MatterWire::Frame request;request.opcode=static_cast<unsigned char>(opcode);request.id=1;
    request.payload.assign(argv[3],argv[3]+std::strlen(argv[3]));
    MatterWire::Bytes bytes;if(!MatterWire::encode(request,&bytes))return 2;
    address.sun_family=AF_UNIX;std::strcpy(address.sun_path,argv[1]);
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return 3;
    timeval timeout;timeout.tv_sec=5;timeout.tv_usec=0;
    if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))||setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))||connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))){close(fd);return 4;}
    unsigned char prefix[2];
    if(!transfer(fd,&bytes[0],bytes.size(),true)||!transfer(fd,prefix,2,false)){close(fd);return 5;}
    unsigned int size=prefix[0]|(static_cast<unsigned int>(prefix[1])<<8);
    if(!size||size>MatterWire::MaxPayload){close(fd);return 6;}
    bytes.assign(prefix,prefix+2);bytes.resize(2+size);
    if(!transfer(fd,&bytes[2],size,false)){close(fd);return 6;}close(fd);
    MatterWire::Decoder decoder;MatterWire::Frame response;
    if(!decoder.feed(bytes)||!decoder.pop(&response)||response.kind!=1||response.opcode!=opcode||response.id!=1)return 6;
    if(fwrite(&response.payload[0],1,response.payload.size(),stdout)!=response.payload.size()||fputc('\n',stdout)==EOF)return 7;
    return 0;
}
