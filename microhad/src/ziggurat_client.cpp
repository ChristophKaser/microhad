#include "ziggurat_client.h"
#include "json_util.h"
#include "mbedtls/sha256.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
typedef ZigguratWire::Bytes Bytes;
const unsigned long kMaxFrame = 16384;
const unsigned long kMaxOutput = 65536;
unsigned int get16(const Bytes& b, unsigned long at) { return b[at] | (static_cast<unsigned int>(b[at+1]) << 8); }
std::string hex(const Bytes& b) {
    const char* digits="0123456789abcdef"; std::string result;
    for (unsigned long i=0;i<b.size();++i) { result += digits[b[i] >> 4]; result += digits[b[i] & 15]; }
    return result;
}
bool nontrivial(const Bytes& bytes, unsigned long at, unsigned long length) {
    bool zero=true, ones=true;
    for (unsigned long i=at;i<at+length;++i) { zero=zero && bytes[i]==0; ones=ones && bytes[i]==255; }
    return !zero && !ones;
}
std::string checksum(const ZigguratState& state) {
    Bytes bytes=state.network; bytes.insert(bytes.end(),state.keys.begin(),state.keys.end());
    bytes.insert(bytes.end(),state.children.begin(),state.children.end());
    bytes.insert(bytes.end(),state.addresses.begin(),state.addresses.end());
    Bytes hash(32,0);
    if (bytes.empty() || mbedtls_sha256_ret(&bytes[0],bytes.size(),&hash[0],0)!=0) return "";
    return hex(hash);
}
bool unhex(const std::string& text, Bytes* result) {
    result->clear(); if (text.size() % 2 || text.size() > 32768) return false;
    for (unsigned long i=0;i<text.size();i+=2) {
        unsigned int value=0;
        for (unsigned int j=0;j<2;++j) {
            char c=text[i+j]; unsigned int v;
            if (c>='0' && c<='9') v=c-'0'; else if (c>='a' && c<='f') v=c-'a'+10; else return false;
            value=value*16+v;
        }
        result->push_back(static_cast<unsigned char>(value));
    }
    return true;
}
bool tableValid(const Bytes& b, unsigned long row, unsigned long maximum) {
    if (b.size()<2 || get16(b,0)>maximum || b.size()!=2+get16(b,0)*row) return false;
    std::map<std::string,bool> identities;
    for (unsigned long at=2;at<b.size();at+=row) {
        unsigned long idAt=at+(row==33 ? 25 : 0);
        std::string id(reinterpret_cast<const char*>(&b[idAt]),8);
        if (identities[id] || !nontrivial(b,idAt,8)) return false;
        if (row==33 && !nontrivial(b,at,16)) return false;
        if (row==11 && (!get16(b,at+8) || get16(b,at+8)>0xfff7 || (b[at+10]&0xf8)!=0 ||
            ((b[at+10]>>1)&3)<1 || ((b[at+10]>>1)&3)>2)) return false;
        if (row==10 && get16(b,at+8)>0xfff7) return false;
        identities[id]=true;
    }
    return true;
}
bool sameIdentity(const Bytes& a, const Bytes& b) {
    if (a.size()!=82 || b.size()!=82) return false;
    for (unsigned long i=0;i<82;++i) {
        if ((i>=39 && i<43) || (i>=78 && i<82)) continue;
        if (a[i]!=b[i]) return false;
    }
    return true;
}
void mergeCounter(Bytes* target, const Bytes& source, unsigned long at) {
    ZigguratWire::set32(target,at,std::max(ZigguratWire::get32(*target,at),ZigguratWire::get32(source,at)));
}
bool fullWrite(int fd, const char* data, unsigned long size) {
    while (size) {
        ssize_t n=write(fd,data,size);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return false;
        data+=n; size-=static_cast<unsigned long>(n);
    }
    return true;
}
}

namespace ZigguratWire {
Bytes encode(const Bytes& plain) {
    Bytes out(1,0); unsigned long codeAt=0; unsigned char code=1;
    for (unsigned long i=0;i<plain.size();++i) {
        if (plain[i]==0) { out[codeAt]=code; codeAt=out.size(); out.push_back(0); code=1; }
        else { out.push_back(plain[i]); ++code; if (code==255) { out[codeAt]=code; codeAt=out.size(); out.push_back(0); code=1; } }
    }
    out[codeAt]=code; out.push_back(0); return out;
}
bool decode(const Bytes& in, Bytes* out) {
    out->clear(); if (in.empty() || in.size()>kMaxFrame) return false;
    unsigned long at=0;
    while (at<in.size()) {
        unsigned char code=in[at++]; if (!code || at+code-1>in.size()) return false;
        for (unsigned int i=1;i<code;++i) { if (!in[at]) return false; out->push_back(in[at++]); }
        if (code!=255 && at<in.size()) out->push_back(0);
    }
    return true;
}
unsigned long get32(const Bytes& b, unsigned long at) {
    return static_cast<unsigned long>(b[at]) | (static_cast<unsigned long>(b[at+1])<<8) |
        (static_cast<unsigned long>(b[at+2])<<16) | (static_cast<unsigned long>(b[at+3])<<24);
}
void put16(Bytes* b, unsigned int value) { b->push_back(static_cast<unsigned char>(value)); b->push_back(static_cast<unsigned char>(value>>8)); }
void set32(Bytes* b, unsigned long at, unsigned long value) { for (unsigned int i=0;i<4;++i) (*b)[at+i]=static_cast<unsigned char>(value>>(i*8)); }
}

bool ZigguratState::valid() const {
    if (network.size()!=82 || network[0]<11 || network[0]>26 || !get16(network,2) || get16(network,2)==65535 ||
        get16(network,12)!=0 || network[59]>1 || network[76]>1) return false;
    if (!nontrivial(network,4,8) || !nontrivial(network,14,8) || !nontrivial(network,22,16) ||
        !nontrivial(network,43,16) || (network[59] && !nontrivial(network,60,16))) return false;
    return tableValid(keys,33,256) && tableValid(children,11,256) && tableValid(addresses,10,256);
}
bool ZigguratState::advance(std::string* error) {
    if (!valid()) { *error="invalid native Ziggurat state"; return false; }
    unsigned long nwk=ZigguratWire::get32(network,39), aps=ZigguratWire::get32(network,78);
    for (unsigned long i=2;i<keys.size();i+=33) aps=std::max(aps,ZigguratWire::get32(keys,i+16));
    if (nwk>0xffffffffUL-1000 || aps>0xffffffffUL-1000) { *error="Ziggurat security counter exhausted"; return false; }
    ZigguratWire::set32(&network,39,nwk+1000); ZigguratWire::set32(&network,78,aps+1000);
    for (unsigned long i=2;i<keys.size();i+=33) ZigguratWire::set32(&keys,i+16,aps+1000);
    return true;
}
bool ZigguratState::load(const std::string& path, std::string* error) {
    int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW); struct stat st;
    if (fd<0) { *error="cannot open private Ziggurat state"; return false; }
    if (fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077)!=0 || st.st_size<1 || st.st_size>65536) {
        close(fd); *error="Ziggurat state must be a private regular file of bounded size"; return false;
    }
    std::string text; char buffer[1024]; ssize_t n;
    while ((n=read(fd,buffer,sizeof(buffer)))>0) { text.append(buffer,n); if (text.size()>65536) break; }
    close(fd);
    long version=0; std::string format, wire, digest; ZigguratState candidate;
    if (n<0 || text.size()>65536 || !jsonStringField(text,"format",&format) || format!="microhad-ziggurat-native" ||
        !jsonLongField(text,"version",&version) || version!=1 ||
        !jsonStringField(text,"network",&wire) || !unhex(wire,&candidate.network) ||
        !jsonStringField(text,"keys",&wire) || !unhex(wire,&candidate.keys) ||
        !jsonStringField(text,"children",&wire) || !unhex(wire,&candidate.children) ||
        !jsonStringField(text,"addresses",&wire) || !unhex(wire,&candidate.addresses) || !candidate.valid() ||
        !jsonStringField(text,"checksum",&digest) || digest.empty() || checksum(candidate)!=digest) {
        *error="invalid native Ziggurat state"; return false;
    }
    *this=candidate; return true;
}
bool ZigguratState::save(const std::string& path, std::string* error) const {
    if (!valid()) { *error="refusing invalid Ziggurat state"; return false; }
    struct stat existing;
    if (lstat(path.c_str(),&existing)==0 && (!S_ISREG(existing.st_mode) || existing.st_uid!=geteuid() || (existing.st_mode&077)!=0)) {
        *error="unsafe Ziggurat state destination"; return false;
    }
    std::string::size_type slash=path.find_last_of('/');
    std::string parent=slash==std::string::npos ? "." : (slash==0 ? "/" : path.substr(0,slash));
    int directory=open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if (directory<0) { *error="cannot open Ziggurat state directory"; return false; }
    std::string temporary=path+".tmp-XXXXXX"; std::vector<char> name(temporary.begin(),temporary.end()); name.push_back(0);
    int fd=mkstemp(&name[0]);
    if (fd<0) { close(directory); *error="cannot stage private Ziggurat state"; return false; }
    std::string data="{\"format\":\"microhad-ziggurat-native\",\"version\":1,\"network\":\""+hex(network)+
        "\",\"keys\":\""+hex(keys)+"\",\"children\":\""+hex(children)+"\",\"addresses\":\""+hex(addresses)+"\",\"checksum\":\""+checksum(*this)+"\"}\n";
    bool ok=fchmod(fd,0600)==0 && fullWrite(fd,data.c_str(),data.size()) && fsync(fd)==0;
    if (close(fd)!=0) ok=false;
    if (ok) ok=rename(&name[0],path.c_str())==0;
    if (ok) ok=fsync(directory)==0;
    close(directory); if (!ok) { unlink(&name[0]); *error="cannot durably commit Ziggurat state"; }
    return ok;
}

ZigguratClient::ZigguratClient() : fd_(-1), lockFd_(-1), connecting_(false), hello_(false), ready_(false), failed_(false),
    discarding_(false), stopping_(false), dirtyTables_(false), nextId_(1), controlId_(0), phase_(0), reconnectAt_(0),
    helloDeadline_(0), checkpointAt_(0), keyRevision_(0), snapshotRevision_(0) {}
ZigguratClient::~ZigguratClient() { stop(); }
bool ZigguratClient::start(const std::string& socket, const std::string& state, std::string* error) {
    if (lockFd_>=0) { *error="Ziggurat client already started"; return false; }
    if (socket.empty() || socket[0]!='/' || socket.size()>=sizeof(sockaddr_un().sun_path)) {
        *error="invalid Ziggurat Unix socket path"; return false;
    }
    statePath_=state; socketPath_=socket;
    lockFd_=open((state+".lock").c_str(),O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    struct stat st; struct flock lock; memset(&lock,0,sizeof(lock)); lock.l_type=F_WRLCK; lock.l_whence=SEEK_SET;
    if (lockFd_<0 || fstat(lockFd_,&st)!=0 || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077)!=0 || fcntl(lockFd_,F_SETLK,&lock)!=0) {
        if (lockFd_>=0) close(lockFd_);
        lockFd_=-1; *error="Ziggurat state is locked or unsafe"; return false;
    }
    if (!state_.load(state,error)) { close(lockFd_); lockFd_=-1; return false; }
    failed_=false; stopping_=false; reconnectAt_=time(0);
    return true;
}
bool ZigguratClient::connectSocket(std::string* error) {
    fd_=socket(AF_UNIX,SOCK_STREAM,0); if (fd_<0) { *error="cannot create Ziggurat socket"; return false; }
    fcntl(fd_,F_SETFD,FD_CLOEXEC); fcntl(fd_,F_SETFL,O_NONBLOCK);
    sockaddr_un address; memset(&address,0,sizeof(address)); address.sun_family=AF_UNIX; strcpy(address.sun_path,socketPath_.c_str());
    int result=connect(fd_,reinterpret_cast<sockaddr*>(&address),sizeof(address));
    if (result<0 && errno!=EINPROGRESS) { close(fd_); fd_=-1; *error="Ziggurat socket unavailable"; return false; }
    connecting_=true; hello_=false; helloDeadline_=time(0)+10; reconnectAt_=0; return true;
}
void ZigguratClient::disconnect(const std::string& reason, bool permanent) {
    if (fd_>=0) close(fd_);
    fd_=-1; ready_=false; connecting_=false; hello_=false; failed_=permanent;
    output_.clear(); input_.clear(); discarding_=false; pending_.clear(); controlId_=0; phase_=0;
    reconnectAt_=permanent || stopping_ ? 0 : time(0)+1; helloDeadline_=0; checkpointAt_=0;
    Event event(Event::Down); event.message=reason; events_.push_back(event);
}
long ZigguratClient::deadline() const {
    long result=fd_<0 ? reconnectAt_ : (hello_ ? checkpointAt_ : helloDeadline_);
    for (std::map<unsigned int,Pending>::const_iterator i=pending_.begin();i!=pending_.end();++i)
        if (!result || i->second.expires<result) result=i->second.expires;
    return result;
}
bool ZigguratClient::pop(Event* event) { if (events_.empty()) return false; *event=events_.front(); events_.pop_front(); return true; }
unsigned int ZigguratClient::request(unsigned char command, const Bytes& payload) {
    if (fd_<0 || !hello_ || pending_.size()>=64 || payload.size()+3>kMaxFrame-66) return 0;
    unsigned int id=0;
    for (unsigned int i=0;i<16383;++i) { unsigned int candidate=nextId_++; if (nextId_==16384) nextId_=1; if (!pending_.count(candidate)) { id=candidate; break; } }
    if (!id) return 0;
    Bytes raw(1,command); ZigguratWire::put16(&raw,id<<2); raw.insert(raw.end(),payload.begin(),payload.end());
    Bytes wire=ZigguratWire::encode(raw); if (output_.size()+wire.size()>kMaxOutput) return 0;
    output_.insert(output_.end(),wire.begin(),wire.end());
    Pending pending; pending.command=command; pending.apsAck=command==0x30 && !payload.empty() && (payload[0]&2)!=0; pending.expires=time(0)+10; pending_[id]=pending; return id;
}
void ZigguratClient::control(unsigned char command, const Bytes& payload, unsigned char phase) {
    phase_=phase; controlId_=request(command,payload);
    if (!controlId_) disconnect("Ziggurat control queue full",true);
}
bool ZigguratClient::persist() {
    std::string error; if (state_.save(statePath_,&error)) return true;
    // Best effort freeze: no guarantee if the server or connection is already lost.
    if (fd_>=0 && hello_) {
        Bytes raw(1,4); ZigguratWire::put16(&raw,16383<<2); Bytes wire=ZigguratWire::encode(raw);
        (void)send(fd_,&wire[0],wire.size(),MSG_NOSIGNAL);
    }
    disconnect(error,true); return false;
}
void ZigguratClient::snapshot(bool initial) {
    snapshot_=state_; snapshot_.keys=Bytes(2,0); snapshot_.children=Bytes(2,0); snapshot_.addresses=Bytes(2,0);
    snapshotRevision_=keyRevision_; dirtyTables_=false;
    control(0x20,Bytes(),initial ? 10 : 20);
}
void ZigguratClient::finishSnapshot() {
    if (!snapshot_.valid()) { disconnect("invalid Ziggurat table snapshot",true); return; }
    if (snapshotRevision_!=keyRevision_) { snapshot(!ready_); return; }
    mergeCounter(&snapshot_.network,state_.network,39); mergeCounter(&snapshot_.network,state_.network,78);
    bool changed=state_.network!=snapshot_.network || state_.keys!=snapshot_.keys ||
        state_.children!=snapshot_.children || state_.addresses!=snapshot_.addresses;
    state_=snapshot_; if (changed && !persist()) return;
    checkpointAt_=time(0)+60; phase_=0; controlId_=0;
    if (!ready_ && !stopping_) { ready_=true; Event event(Event::Ready); events_.push_back(event); }
}
void ZigguratClient::controlReply(unsigned char command, unsigned char status, const Bytes& body) {
    (void)command;
    if (phase_==1 && status==0x10) {
        std::string error; if (!state_.advance(&error)) { disconnect(error,true); return; }
        if (!persist()) return;
        Bytes config(1,0); config.push_back(1); config.insert(config.end(),state_.network.begin(),state_.network.end()); control(0x10,config,2); return;
    }
    if (status) { disconnect("Ziggurat lifecycle request failed",true); return; }
    if (phase_==1) {
        if (body.size()!=85) { disconnect("malformed Ziggurat network info",true); return; }
        Bytes network(body.begin(),body.begin()+82);
        if (!sameIdentity(network,state_.network)) { disconnect("Ziggurat network identity mismatch",true); return; }
        if (ZigguratWire::get32(network,39)<ZigguratWire::get32(state_.network,39) ||
            ZigguratWire::get32(network,78)<ZigguratWire::get32(state_.network,78)) { disconnect("Ziggurat live counters below saved state",true); return; }
        state_.network=network;
        if (body[84]==0) {
            std::string error; if (!state_.advance(&error)) { disconnect(error,true); return; }
            if (!persist()) return;
            Bytes config(1,0); config.push_back(1); config.insert(config.end(),state_.network.begin(),state_.network.end()); control(0x10,config,2);
        } else snapshot(true);
    } else if (phase_==2) control(0x11,state_.keys,3);
    else if (phase_==3) control(0x12,state_.children,4);
    else if (phase_==4) control(0x13,state_.addresses,5);
    else if (phase_==5) control(0x16,Bytes(),6);
    else if (phase_==6) snapshot(true);
    else if (phase_==30) snapshot(false);
    else if (phase_==10 || phase_==20) {
        if (body.size()!=85) { disconnect("malformed Ziggurat network info",true); return; }
        Bytes network(body.begin(),body.begin()+82);
        if (!sameIdentity(network,state_.network)) { disconnect("Ziggurat snapshot identity changed",true); return; }
        snapshot_.network=network;
        control(0x21,Bytes(),phase_==10 ? 11 : 21);
    } else if (phase_==11 || phase_==21) control(0x22,Bytes(),phase_==11 ? 12 : 22);
    else if (phase_==12 || phase_==22) control(0x23,Bytes(),phase_==12 ? 13 : 23);
    else if (phase_==13 || phase_==23) control(0x20,Bytes(),phase_==13 ? 14 : 24);
    else if (phase_==14 || phase_==24) {
        if (body.size()!=85) { disconnect("malformed Ziggurat final snapshot",true); return; }
        Bytes network(body.begin(),body.begin()+82);
        if (!sameIdentity(network,state_.network)) { disconnect("Ziggurat final identity changed",true); return; }
        mergeCounter(&snapshot_.network,network,39); mergeCounter(&snapshot_.network,network,78); finishSnapshot();
    }
}
void ZigguratClient::frame(const Bytes& raw) {
    if (events_.size()>=256) { disconnect("Ziggurat notification queue exceeded",true); return; }
    if (raw.size()<3) return;
    unsigned char command=raw[0]; unsigned int bits=get16(raw,1), type=bits&3, id=bits>>2;
    Bytes body(raw.begin()+3,raw.end());
    if (type==3 && command==0 && id==0) {
        if (body.size()!=2 || body[0]!=2 || hello_) { disconnect("unsupported Ziggurat hello",true); return; }
        hello_=true; helloDeadline_=0; control(0x20,Bytes(),1); return;
    }
    if (!hello_) { disconnect("Ziggurat frame before hello",true); return; }
    if (id==controlId_ && controlId_ && type==2) {
        Bytes* table=command==0x21 ? &snapshot_.keys : command==0x22 ? &snapshot_.children : command==0x23 ? &snapshot_.addresses : 0;
        unsigned long width=command==0x21 ? 33 : command==0x22 ? 11 : 10;
        if (!table || body.size()!=width || get16(*table,0)>=256) { disconnect("invalid Ziggurat scan event",true); return; }
        unsigned int count=get16(*table,0)+1; (*table)[0]=static_cast<unsigned char>(count); (*table)[1]=static_cast<unsigned char>(count>>8);
        table->insert(table->end(),body.begin(),body.end()); return;
    }
    if (type==1) {
        std::map<unsigned int,Pending>::iterator found=pending_.find(id);
        if (found==pending_.end()) return;
        if (found->second.command!=command || body.empty()) { disconnect("mismatched Ziggurat response",true); return; }
        unsigned char status=body[0]; body.erase(body.begin());
        if (id==controlId_) {
            pending_.erase(found); controlId_=0;
            controlReply(command,status,body); return;
        }
        bool sendCommand=command>=0x30 && command<=0x32;
        Event event(sendCommand ? Event::Admission : Event::JoinReply); event.id=id; event.command=command; event.status=status;
        events_.push_back(event);
        if (sendCommand && !status) { found->second.admitted=true; found->second.expires=time(0)+30; }
        else pending_.erase(found);
        return;
    }
    if (type!=3) return;
    if (command>=0x11 && command<=0x13 && id) {
        std::map<unsigned int,Pending>::iterator found=pending_.find(id);
        if (found==pending_.end() || body.size()!=1) return;
        unsigned char sendCommand=found->second.command;
        if ((command==0x13 && sendCommand!=0x30) || (command==0x11 && sendCommand==0x30) || (command==0x12 && sendCommand==0x30)) {
            Event event(Event::Confirmation); event.id=id; event.command=command; event.status=body[0]; events_.push_back(event);
            if (body[0] || command!=0x11 || !found->second.apsAck) pending_.erase(found);
        }
        return;
    }
    if (id) return;
    if (command==0x30 || command==0x31) {
        if (body.size()!=4) { disconnect("invalid counter notification",true); return; }
        unsigned long at=command==0x30 ? 39 : 78;
        unsigned long value=ZigguratWire::get32(body,0);
        if (value>ZigguratWire::get32(state_.network,at)) { ZigguratWire::set32(&state_.network,at,value); persist(); }
        return;
    }
    if (command==0x32) {
        if (body.size()!=24) { disconnect("invalid key notification",true); return; }
        Bytes keys(2,0); unsigned int count=0;
        for (unsigned long at=2;at<state_.keys.size();at+=33) {
            if (std::equal(body.begin(),body.begin()+8,state_.keys.begin()+at+25)) continue;
            keys.insert(keys.end(),state_.keys.begin()+at,state_.keys.begin()+at+33); ++count;
        }
        Bytes row(body.begin()+8,body.end()); row.resize(25,0); ZigguratWire::set32(&row,16,ZigguratWire::get32(state_.network,78));
        row.insert(row.end(),body.begin(),body.begin()+8); keys.insert(keys.end(),row.begin(),row.end()); ++count;
        keys[0]=static_cast<unsigned char>(count); keys[1]=static_cast<unsigned char>(count>>8); state_.keys=keys; ++keyRevision_;
        dirtyTables_=true; persist(); return;
    }
    Event event(command==0x10 ? Event::ReceivedAps : command==0x20 ? Event::Joined : command==0x21 ? Event::Left : Event::Diagnostic);
    event.command=command; event.payload=body; events_.push_back(event);
    if (command==0x20 || command==0x21) dirtyTables_=true;
}
void ZigguratClient::poll() {
    if (failed_ || lockFd_<0) return;
    if (fd_<0) {
        if (reconnectAt_ && time(0)>=reconnectAt_) { std::string error; if (!connectSocket(&error)) reconnectAt_=time(0)+1; }
        if (fd_<0) return;
    }
    if (connecting_) {
        int error=0; socklen_t size=sizeof(error);
        if (getsockopt(fd_,SOL_SOCKET,SO_ERROR,&error,&size)!=0 || error) { disconnect("Ziggurat connect failed"); return; }
        struct PeerCredentials { pid_t pid; uid_t uid; gid_t gid; } credentials;
        socklen_t credentialsSize=sizeof(credentials);
        if (getsockopt(fd_,SOL_SOCKET,SO_PEERCRED,&credentials,&credentialsSize)!=0 || credentials.uid!=geteuid()) {
            disconnect("Ziggurat socket peer has a different owner",true); return;
        }
        connecting_=false;
    }
    if (!output_.empty()) {
        ssize_t n=send(fd_,&output_[0],output_.size(),MSG_NOSIGNAL);
        if (n>0) output_.erase(output_.begin(),output_.begin()+n);
        else if (n<0 && errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK) { disconnect("Ziggurat write failed"); return; }
    }
    unsigned char buffer[1024];
    for (unsigned int reads=0;reads<16;++reads) {
        ssize_t n=read(fd_,buffer,sizeof(buffer));
        if (n==0) { disconnect("Ziggurat disconnected; pending sends have uncertain delivery"); return; }
        if (n<0) { if (errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK) disconnect("Ziggurat read failed"); break; }
        for (ssize_t i=0;i<n;++i) {
            unsigned char byte=buffer[i];
            if (!byte) {
                if (discarding_) discarding_=false;
                else if (!input_.empty()) { Bytes decoded; if (ZigguratWire::decode(input_,&decoded)) frame(decoded); }
                input_.clear(); if (fd_<0) return;
            } else if (!discarding_) {
                if (input_.size()>=kMaxFrame) { input_.clear(); discarding_=true; }
                else input_.push_back(byte);
            }
        }
    }
    if (!hello_ && helloDeadline_ && time(0)>=helloDeadline_) { disconnect("Ziggurat hello timed out"); return; }
    for (std::map<unsigned int,Pending>::iterator i=pending_.begin();i!=pending_.end();) {
        if (time(0)<i->second.expires) { ++i; continue; }
        if (i->first==controlId_) { disconnect("Ziggurat lifecycle timed out"); return; }
        Event event(i->second.command>=0x30 && i->second.command<=0x32 ? Event::Confirmation : Event::JoinReply);
        event.id=i->first; event.command=i->second.command; event.status=0xff; events_.push_back(event);
        pending_.erase(i++);
    }
    if (ready_ && !controlId_ && !stopping_ && (dirtyTables_ || (checkpointAt_ && time(0)>=checkpointAt_))) snapshot(false);
}
void ZigguratClient::stop() {
    if (lockFd_<0) return;
    stopping_=true;
    if (fd_>=0 && ready_ && !failed_) {
        // Finish an admitted checkpoint before freezing; never abandon its ID.
        long end=time(0)+5;
        while (fd_>=0 && controlId_ && time(0)<end) { poll(); usleep(1000); }
        if (fd_>=0 && !controlId_) {
            control(4,Bytes(),30);
            while (fd_>=0 && controlId_ && time(0)<end) { poll(); usleep(1000); }
        }
    }
    if (fd_>=0) disconnect("Ziggurat client stopped");
    close(lockFd_); lockFd_=-1;
}
