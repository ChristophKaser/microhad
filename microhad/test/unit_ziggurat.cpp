#include "ziggurat_client.h"
#include <cassert>
#include <cstdlib>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

void runZigguratUnitTests() {
    typedef ZigguratWire::Bytes Bytes;
    const unsigned char knownRaw[]={0x11,0,0x22}, knownWire[]={2,0x11,2,0x22,0};
    assert(ZigguratWire::encode(Bytes(knownRaw,knownRaw+3))==Bytes(knownWire,knownWire+5));
    Bytes raw;
    for (unsigned int i=0;i<1024;++i) raw.push_back(static_cast<unsigned char>(i));
    Bytes wire=ZigguratWire::encode(raw), decoded;
    assert(wire.back()==0); wire.pop_back();
    assert(ZigguratWire::decode(wire,&decoded) && raw==decoded);
    assert(!ZigguratWire::decode(Bytes(1,0),&decoded));
    assert(!ZigguratWire::decode(Bytes(1,2),&decoded));
    assert(!ZigguratWire::decode(Bytes(16385,1),&decoded));
    ZigguratState state; state.network=Bytes(82,0); state.network[0]=15; state.network[2]=1;
    state.keys=state.children=state.addresses=Bytes(2,0);
    assert(!state.valid());
    state.network[4]=2; state.network[14]=2; state.network[22]=2; state.network[43]=2;
    assert(state.valid()); std::string error;
    assert(state.advance(&error)); assert(ZigguratWire::get32(state.network,39)==1000);
    assert(ZigguratWire::get32(state.network,78)==1000);
    ZigguratWire::set32(&state.network,39,0xffffffffUL); assert(!state.advance(&error));
    ZigguratWire::set32(&state.network,39,1000);
    char directory[]="/tmp/microhad-ziggurat-state-test-XXXXXX"; assert(mkdtemp(directory));
    std::string path=std::string(directory)+"/state.json";
    assert(state.save(path,&error)); struct stat st; assert(stat(path.c_str(),&st)==0 && (st.st_mode&0777)==0600);
    ZigguratState loaded; assert(loaded.load(path,&error) && loaded.network==state.network);
    int fd=open(path.c_str(),O_RDWR); assert(fd>=0);
    // A valid-width counter change must fail checksum verification.
    off_t offset=0; char data[2048]; ssize_t size=read(fd,data,sizeof(data)); assert(size>0);
    std::string text(data,size); offset=static_cast<off_t>(text.find("network")+11+39*2);
    assert(lseek(fd,offset,SEEK_SET)==offset); assert(write(fd,"f",1)==1); close(fd);
    assert(!loaded.load(path,&error)); assert(state.save(path,&error));
    ZigguratClient owner; assert(owner.start(std::string(directory)+"/api.sock",path,&error));
    pid_t child=fork(); assert(child>=0);
    if (child==0) { ZigguratClient competitor; _exit(competitor.start(std::string(directory)+"/api.sock",path,&error) ? 1 : 0); }
    int status=0; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    owner.stop(); assert(unlink((path+".lock").c_str())==0);
    assert(chmod(path.c_str(),0644)==0); assert(!loaded.load(path,&error)); assert(!state.save(path,&error));
    assert(unlink(path.c_str())==0); assert(symlink("/dev/null",path.c_str())==0);
    assert(!loaded.load(path,&error)); assert(!state.save(path,&error));
    assert(unlink(path.c_str())==0); assert(rmdir(directory)==0);
}
