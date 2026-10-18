#include "companion_discovery.h"
#include <cassert>
#include <arpa/inet.h>
#include <cstring>
void runCompanionDiscoveryUnitTests() {
    size_t at=0; std::string name;
    assert(!CompanionDiscovery::readName("",&at,&name));
    std::string cycle("\xc0\x00",2); at=0;
    assert(!CompanionDiscovery::readName(cycle,&at,&name));
    std::string truncated("\x05" "abc",4); at=0;
    assert(!CompanionDiscovery::readName(truncated,&at,&name));
    std::string mixed("\x03" "AbC\x00\xc0\x00",7); at=5;
    assert(CompanionDiscovery::readName(mixed,&at,&name) && name=="abc" && at==7);
    uint32_t address=inet_addr("192.0.2.1");
    std::string packet=CompanionDiscovery::records("test-gateway",address,8080,120);
    assert(packet.size()<1500 && static_cast<unsigned char>(packet[2])==0x84 && packet[7]==4);
    at=12;
    for (unsigned int i=0;i<4;++i) {
        assert(CompanionDiscovery::readName(packet,&at,&name));
        assert(at+10<=packet.size());
        unsigned int type=static_cast<unsigned char>(packet[at+1]);
        unsigned int length=(static_cast<unsigned char>(packet[at+8])<<8)|static_cast<unsigned char>(packet[at+9]);
        assert(static_cast<unsigned char>(packet[at+7])==120); at+=10;
        assert(at+length<=packet.size());
        if (type==33) { assert(static_cast<unsigned char>(packet[at+4])==31 && static_cast<unsigned char>(packet[at+5])==144); }
        if (type==1) assert(length==4 && memcmp(packet.data()+at,&address,4)==0);
        at+=length;
    }
    assert(at==packet.size());
    std::string goodbye=CompanionDiscovery::records("test-gateway",address,8080,0);
    assert(goodbye.size()==packet.size() && goodbye!=packet);
    assert(CompanionDiscovery::records(std::string(64,'x'),address,80,120).empty());
}
