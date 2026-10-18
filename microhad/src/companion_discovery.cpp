#include "companion_discovery.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace {
uint64_t millis() {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
void word(std::string* out, unsigned int value) { out->push_back(static_cast<char>(value >> 8)); out->push_back(static_cast<char>(value)); }
unsigned int getWord(const std::string& in, size_t at) { return (static_cast<unsigned char>(in[at]) << 8) | static_cast<unsigned char>(in[at+1]); }
void name(std::string* out, const std::string& value) {
    size_t at = 0;
    while (at < value.size()) {
        size_t end = value.find('.', at);
        if (end == std::string::npos) end = value.size();
        out->push_back(static_cast<char>(end-at)); out->append(value, at, end-at); at = end+1;
    }
    out->push_back(0);
}
void record(std::string* out, const std::string& owner, unsigned int type, unsigned long ttl, const std::string& data, bool unique) {
    name(out, owner); word(out, type); word(out, unique ? 0x8001 : 1);
    word(out, static_cast<unsigned int>(ttl >> 16)); word(out, static_cast<unsigned int>(ttl));
    word(out, static_cast<unsigned int>(data.size())); out->append(data);
}
std::string instance(const std::string& label) { return label + "._microhad._tcp.local"; }
std::string host(const std::string& label) { return label + ".local"; }
std::string labelFor(uint32_t address, unsigned int suffix) {
    char buffer[64]; snprintf(buffer, sizeof(buffer), "microhad-%08lx-%u", static_cast<unsigned long>(ntohl(address)), suffix);
    return buffer;
}
}
CompanionDiscovery::CompanionDiscovery(unsigned short port) : fd_(-1), port_(port), refresh_(0), lastReply_(0) {}
CompanionDiscovery::~CompanionDiscovery() { closeSocket(); }
bool CompanionDiscovery::readName(const std::string& packet, size_t* offset, std::string* result) {
    size_t at = *offset; bool jumped = false; result->clear();
    for (unsigned int steps = 0; steps < 128 && at < packet.size(); ++steps) {
        unsigned int length = static_cast<unsigned char>(packet[at++]);
        if ((length & 0xc0) == 0xc0) {
            if (at >= packet.size()) return false;
            size_t target = ((length & 63) << 8) | static_cast<unsigned char>(packet[at++]);
            if (!jumped) *offset = at;
            jumped = true; at = target; continue;
        }
        if (length == 0) { if (!jumped) *offset = at; return true; }
        if (length > 63 || at + length > packet.size() || result->size() + length + 1 > 254) return false;
        if (!result->empty()) result->push_back('.');
        for (unsigned int i=0; i<length; ++i) {
            unsigned char ch = static_cast<unsigned char>(packet[at++]);
            result->push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a'-'A') : ch));
        }
    }
    return false;
}
std::string CompanionDiscovery::records(const std::string& label, uint32_t address, unsigned short port, unsigned long ttl) {
    if (label.empty() || label.size() > 63 || label.find('.') != std::string::npos) return "";
    std::string out(12, '\0'); out[2] = static_cast<char>(0x84); out[7] = 4;
    std::string data; name(&data, instance(label)); record(&out, "_microhad._tcp.local", 12, ttl, data, false);
    data.clear(); word(&data, 0); word(&data, 0); word(&data, port); name(&data, host(label));
    record(&out, instance(label), 33, ttl, data, true);
    data.assign(1, static_cast<char>(8)); data += "schema=1";
    record(&out, instance(label), 16, ttl, data, true);
    data.assign(reinterpret_cast<const char*>(&address), 4); record(&out, host(label), 1, ttl, data, true);
    return out;
}
void CompanionDiscovery::send(const Interface& interface, const std::string& packet) {
    struct in_addr source; source.s_addr = interface.address;
    if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &source, sizeof(source)) != 0) return;
    struct sockaddr_in destination; memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET; destination.sin_port = htons(5353); destination.sin_addr.s_addr = inet_addr("224.0.0.251");
    sendto(fd_, packet.data(), packet.size(), 0, reinterpret_cast<struct sockaddr*>(&destination), sizeof(destination));
}
void CompanionDiscovery::closeSocket() {
    if (fd_ >= 0) {
        for (size_t i=0; i<interfaces_.size(); ++i) if (interfaces_[i].probes == 3)
            send(interfaces_[i], records(interfaces_[i].label, interfaces_[i].address, port_, 0));
        close(fd_); fd_ = -1;
    }
    interfaces_.clear();
}
void CompanionDiscovery::refresh(uint64_t now) {
    refresh_ = now + 10000;
    int query = socket(AF_INET, SOCK_DGRAM, 0); if (query < 0) return;
    struct ifreq entries[32]; struct ifconf config;
    memset(&config, 0, sizeof(config)); config.ifc_len = sizeof(entries); config.ifc_req = entries;
    std::vector<uint32_t> addresses;
    if (ioctl(query, SIOCGIFCONF, &config) == 0) {
        size_t count = static_cast<size_t>(config.ifc_len) / sizeof(struct ifreq);
        for (size_t i=0; i<count && addresses.size()<8; ++i) {
            if (entries[i].ifr_addr.sa_family != AF_INET) continue;
            uint32_t address = reinterpret_cast<struct sockaddr_in*>(&entries[i].ifr_addr)->sin_addr.s_addr;
            struct ifreq flags = entries[i];
            if (ioctl(query, SIOCGIFFLAGS, &flags) != 0 || !(flags.ifr_flags & IFF_UP) ||
                !(flags.ifr_flags & IFF_MULTICAST) || (flags.ifr_flags & IFF_LOOPBACK)) continue;
            if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) addresses.push_back(address);
        }
    }
    close(query);
    bool same = addresses.size() == interfaces_.size();
    for (size_t i=0; same && i<addresses.size(); ++i) same = addresses[i] == interfaces_[i].address;
    if (same && fd_ >= 0) return;
    closeSocket(); if (addresses.empty()) return;
    fd_ = socket(AF_INET, SOCK_DGRAM, 0); if (fd_ < 0) return;
    int one = 1; unsigned char ttl = 255;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    struct sockaddr_in bindAddress; memset(&bindAddress, 0, sizeof(bindAddress));
    bindAddress.sin_family = AF_INET; bindAddress.sin_port = htons(5353);
    if (bind(fd_, reinterpret_cast<struct sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0 ||
        fcntl(fd_, F_SETFL, O_NONBLOCK) != 0) { closeSocket(); return; }
    for (size_t i=0; i<addresses.size(); ++i) {
        struct ip_mreq membership; membership.imr_multiaddr.s_addr = inet_addr("224.0.0.251"); membership.imr_interface.s_addr = addresses[i];
        if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0) continue;
        Interface interface; interface.address=addresses[i]; interface.suffix=1; interface.label=labelFor(addresses[i],1);
        interface.probes=0; interface.due=now+250; interfaces_.push_back(interface);
    }
}
void CompanionDiscovery::poll() {
    uint64_t now = millis(); if (now >= refresh_) refresh(now); if (fd_ < 0) return;
    for (size_t i=0; i<interfaces_.size(); ++i) {
        Interface& interface=interfaces_[i]; if (now < interface.due) continue;
        if (interface.probes < 3) {
            std::string probe(12, '\0'); probe[5]=2;
            name(&probe, instance(interface.label)); word(&probe,255); word(&probe,1);
            name(&probe, host(interface.label)); word(&probe,255); word(&probe,1);
            send(interface,probe); ++interface.probes; interface.due=now+250;
        } else { send(interface,records(interface.label,interface.address,port_,120)); interface.due=now+60000; }
    }
    for (unsigned int received=0; received<8; ++received) {
        char buffer[1501]; struct sockaddr_in source; socklen_t sourceSize=sizeof(source);
        ssize_t count=recvfrom(fd_,buffer,sizeof(buffer),0,reinterpret_cast<struct sockaddr*>(&source),&sourceSize);
        if (count<0) break;
        if (count<12 || count>1500 || source.sin_port!=htons(5353)) continue;
        std::string packet(buffer,static_cast<size_t>(count));
        unsigned int questions=getWord(packet,4), answers=getWord(packet,6)+getWord(packet,8)+getWord(packet,10);
        if (questions>32 || answers>64) continue;
        size_t at=12; bool valid=true, wanted=false;
        for (unsigned int q=0; q<questions; ++q) {
            std::string owner;
            if (!readName(packet,&at,&owner) || at+4>packet.size()) { valid=false; break; }
            unsigned int type=getWord(packet,at), klass=getWord(packet,at+2)&0x7fff; at+=4;
            if (klass!=1) continue;
            if (owner=="_microhad._tcp.local" && (type==12 || type==255)) wanted=true;
            for (size_t i=0; i<interfaces_.size(); ++i)
                if (owner==host(interfaces_[i].label) || owner==instance(interfaces_[i].label)) wanted=true;
        }
        if (!valid) continue;
        for (unsigned int a=0; a<answers; ++a) {
            std::string owner;
            if (!readName(packet,&at,&owner) || at+10>packet.size()) { valid=false; break; }
            unsigned int type=getWord(packet,at), length=getWord(packet,at+8); at+=10;
            if (at+length>packet.size()) { valid=false; break; }
            for (size_t i=0; i<interfaces_.size(); ++i) {
                Interface& interface=interfaces_[i]; bool conflict=false;
                if (type==1 && length==4 && owner==host(interface.label)) conflict=memcmp(packet.data()+at,&interface.address,4)!=0;
                if (type==33 && length>=7 && owner==instance(interface.label)) {
                    size_t target=at+6; std::string targetName;
                    if (!readName(packet,&target,&targetName)) { valid=false; break; }
                    conflict=getWord(packet,at+4)!=port_ || targetName!=host(interface.label);
                }
                if (conflict) { interface.label=labelFor(interface.address,++interface.suffix); interface.probes=0; interface.due=now+250; }
            }
            at+=length;
        }
        if (valid && !(static_cast<unsigned char>(packet[2])&0x80) && wanted && now-lastReply_>=250) {
            for (size_t i=0; i<interfaces_.size(); ++i) if (interfaces_[i].probes==3 && now>=interfaces_[i].due-250)
                send(interfaces_[i],records(interfaces_[i].label,interfaces_[i].address,port_,120));
            // Announced interfaces have due in the future; reply to them as well.
            for (size_t i=0; i<interfaces_.size(); ++i) if (interfaces_[i].probes==3 && interfaces_[i].due>now+250)
                send(interfaces_[i],records(interfaces_[i].label,interfaces_[i].address,port_,120));
            lastReply_=now;
        }
    }
}
