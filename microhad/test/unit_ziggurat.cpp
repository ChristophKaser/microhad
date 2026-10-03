#include "ziggurat_client.h"
#include <cassert>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

void runZigguratUnitTests() {
    typedef ZigguratWire::Bytes Bytes;
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
    assert(state.valid()); std::string error;
    assert(state.advance(&error)); assert(ZigguratWire::get32(state.network,39)==1000);
    assert(ZigguratWire::get32(state.network,78)==1000);
    ZigguratWire::set32(&state.network,39,0xffffffffUL); assert(!state.advance(&error));
    ZigguratWire::set32(&state.network,39,1000);
    char directory[]="/tmp/microhad-ziggurat-state-test-XXXXXX"; assert(mkdtemp(directory));
    std::string path=std::string(directory)+"/state.json";
    assert(state.save(path,&error)); struct stat st; assert(stat(path.c_str(),&st)==0 && (st.st_mode&0777)==0600);
    ZigguratState loaded; assert(loaded.load(path,&error) && loaded.network==state.network);
    assert(chmod(path.c_str(),0644)==0); assert(!loaded.load(path,&error)); assert(!state.save(path,&error));
    assert(unlink(path.c_str())==0); assert(symlink("/dev/null",path.c_str())==0);
    assert(!loaded.load(path,&error)); assert(!state.save(path,&error));
    assert(unlink(path.c_str())==0); assert(rmdir(directory)==0);
}
