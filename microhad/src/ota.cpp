#include "ota.h"
#include "json_util.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/statvfs.h>

namespace {
struct Lock {
    pthread_mutex_t* m;
    bool held;
    Lock(pthread_mutex_t* p):m(p),held(true){pthread_mutex_lock(m);}
    void release(){if(held){pthread_mutex_unlock(m);held=false;}}
    void acquire(){if(!held){pthread_mutex_lock(m);held=true;}}
    ~Lock(){release();}
};
unsigned long le(const unsigned char* p, unsigned n) { unsigned long v=0; for(unsigned i=0;i<n;++i)v|=static_cast<unsigned long>(p[i])<<(8*i); return v; }
void put(std::vector<unsigned char>* p,unsigned long v,unsigned n){for(unsigned i=0;i<n;++i)p->push_back(static_cast<unsigned char>(v>>(8*i)));}
bool rpc(const std::string& socket,const std::string& body,std::string* result) {
    if(socket.size()>=sizeof(((sockaddr_un*)0)->sun_path))return false;
    int fd=::socket(AF_UNIX,SOCK_STREAM,0); if(fd<0)return false;
    timeval timeout;timeout.tv_sec=2;timeout.tv_usec=0;
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    sockaddr_un a;memset(&a,0,sizeof(a));a.sun_family=AF_UNIX;strcpy(a.sun_path,socket.c_str());
    bool ok=connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0;
    std::string request=body+"\n";unsigned long sent=0;
    while(ok&&sent<request.size()){ssize_t n=send(fd,request.data()+sent,request.size()-sent,MSG_NOSIGNAL);if(n<=0)ok=false;else sent+=n;}
    result->clear(); char b[1024];
    while(ok&&result->find('\n')==std::string::npos&&result->size()<4096){ssize_t n=read(fd,b,sizeof(b));if(n<=0){ok=false;break;}result->append(b,n);}
    close(fd);return ok&&result->find('\n')!=std::string::npos;
}
}
OtaStore& OtaStore::instance(){static OtaStore store;return store;}
OtaStore::OtaStore():fd_(-1),state_("empty"),size_(0),received_(0),progress_(0),manufacturer_(0),type_(0),minHardware_(0),maxHardware_(0),version_(0),hardwareRange_(false),zigbeeOffered_(false),touched_(0){pthread_mutex_init(&mutex_,0);}
OtaStore::~OtaStore(){clear();pthread_mutex_destroy(&mutex_);}
void OtaStore::clear(){if(fd_>=0)close(fd_);fd_=-1;if(!path_.empty())unlink(path_.c_str());path_.clear();id_.clear();protocol_.clear();state_="empty";target_.clear();error_.clear();matterSocket_.clear();size_=received_=progress_=0;manufacturer_=type_=minHardware_=maxHardware_=0;version_=0;hardwareRange_=false;zigbeeOffered_=false;touched_=0;}
bool OtaStore::active() const{return state_=="starting"||state_=="cancelling"||state_=="waiting"||state_=="transferring"||state_=="awaiting_confirmation";}
void OtaStore::expire(){if(!matterSocket_.empty())return;if(touched_&&time(0)-touched_>3600&&(active()||state_=="uploading")){state_="failed";error_="Update timed out; wake the device and start again";}}
bool OtaStore::upload(const std::string& action,const std::string& body,std::string* error){
    Lock lock(&mutex_);expire();std::string image;jsonStringField(body,"image",&image);
    if(action=="begin"){
        long size=0;std::string protocol;
        if(active()||state_=="uploading"){*error="An upload or installation is already active";return false;}
        if(!jsonLongField(body,"size",&size)||size<56||size>16*1024*1024||!jsonStringField(body,"protocol",&protocol)||(protocol!="zigbee"&&protocol!="matter")){*error="Invalid firmware size or protocol (maximum 16 MiB)";return false;}
        struct statvfs fs;if(statvfs("/tmp",&fs)!=0||static_cast<uint64_t>(fs.f_bavail)*fs.f_frsize<static_cast<uint64_t>(size)+1024*1024){*error="Insufficient temporary storage";return false;}
        clear();char path[]="/tmp/microhad-ota-XXXXXX";fd_=mkstemp(path);if(fd_<0){*error="Cannot create temporary firmware file";return false;}
        fcntl(fd_,F_SETFD,FD_CLOEXEC);path_=path;id_=path_.substr(path_.size()-6);protocol_=protocol;size_=size;state_="uploading";touched_=time(0);return true;
    }
    if(image!=id_||id_.empty()||state_!="uploading"){*error="Upload is no longer available";return false;}
    if(action=="chunk"){
        long offset=-1;std::string hex;
        if(!jsonLongField(body,"offset",&offset)||offset<0||static_cast<unsigned long>(offset)!=received_||!jsonStringField(body,"data",&hex)||hex.empty()||hex.size()>8192||hex.size()%2||hex.find_first_not_of("0123456789abcdef")!=std::string::npos||hex.size()/2>size_-received_){*error="Invalid firmware chunk or offset";return false;}
        std::string bytes;for(unsigned long i=0;i<hex.size();i+=2)bytes+=static_cast<char>(strtoul(hex.substr(i,2).c_str(),0,16));
        unsigned long at=0;while(at<bytes.size()){ssize_t n=write(fd_,bytes.data()+at,bytes.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0){clear();*error="Firmware write failed; upload discarded";return false;}at+=n;}
        received_+=bytes.size();touched_=time(0);return true;
    }
    if(action=="finish"&&received_==size_){if(!parse(error)){clear();return false;}state_="ready";return true;}
    *error="Incomplete firmware upload";return false;
}
bool OtaStore::parse(std::string* error){
    unsigned char h[128];ssize_t n=pread(fd_,h,sizeof(h),0);
    if(protocol_=="matter"){
        if(n>=16&&le(h,4)==0x1beef11eUL&&le(h+4,4)==size_&&le(h+8,4)==0&&le(h+12,4)<=4096&&16+le(h+12,4)<size_)return true;
        *error="Invalid Matter OTA container";return false;
    }
    if(n<56||le(h,4)!=0x0beef11eUL||le(h+4,2)!=0x0100||le(h+52,4)!=size_){*error="Invalid Zigbee OTA container";return false;}
    unsigned flags=le(h+8,2),header=le(h+6,2);unsigned expected=56+((flags&1)?1:0)+((flags&2)?8:0)+((flags&4)?4:0);
    if((flags&~7)||header!=expected||header>=size_||(flags&2)){*error="Unsupported Zigbee OTA header or device-specific image";return false;}
    manufacturer_=le(h+10,2);type_=le(h+12,2);version_=le(h+14,4);hardwareRange_=(flags&4)!=0;
    if(hardwareRange_){unsigned at=56+((flags&1)?1:0);minHardware_=le(h+at,2);maxHardware_=le(h+at+2,2);if(minHardware_>maxHardware_){*error="Invalid hardware version range";return false;}}
    unsigned long at=header;while(at<size_){unsigned char tag[6];if(size_-at<6||pread(fd_,tag,6,at)!=6){*error="Truncated OTA sub-element";return false;}unsigned long length=le(tag+2,4);at+=6;if(length>size_-at){*error="Invalid OTA sub-element size";return false;}at+=length;}
    return true;
}
std::string OtaStore::status(){Lock lock(&mutex_);expire();
    if(!matterSocket_.empty()&&active()&&state_!="starting"&&state_!="cancelling"){
        const std::string socket=matterSocket_,image=id_;std::string reply;
        lock.release();bool ok=rpc(socket,"{\"action\":\"status\"}",&reply);lock.acquire();
        if(id_==image&&active()&&state_!="starting"&&state_!="cancelling"){
            if(ok){std::string state,error;long progress=0;
                if(jsonStringField(reply,"state",&state))state_=state;
                if(jsonStringField(reply,"error",&error))error_=error;
                if(jsonLongField(reply,"progress",&progress)&&progress>=0)progress_=progress;
            }else{error_="Matter OTA service unavailable; installation state is unknown";}
        }
    }
    std::ostringstream out;out<<"{\"image\":\""<<id_<<"\",\"protocol\":\""<<protocol_<<"\",\"state\":\""<<state_<<"\",\"target\":\""<<jsonEscape(target_)<<"\",\"size\":"<<size_<<",\"uploaded\":"<<received_<<",\"progress\":"<<progress_<<",\"version\":"<<version_<<",\"manufacturer\":"<<manufacturer_<<",\"image_type\":"<<type_<<",\"error\":\""<<jsonEscape(error_)<<"\"}";return out.str();}
bool OtaStore::startZigbee(const std::string& image,const std::string& address,std::string* error){Lock lock(&mutex_);expire();if(image!=id_||protocol_!="zigbee"||state_!="ready"){*error="Upload a Zigbee image before starting";return false;}target_=address;state_="waiting";progress_=0;zigbeeOffered_=false;touched_=time(0);return true;}
bool OtaStore::cancel(const std::string& image,bool discard,std::string* error){
    Lock lock(&mutex_);
    if(image!=id_||id_.empty()){*error="Unknown firmware image";return false;}
    if(state_=="starting"||state_=="cancelling"){*error="OTA operation is in progress";return false;}
    if(!matterSocket_.empty()){
        const std::string socket=matterSocket_,previous=state_;state_="cancelling";std::string reply;
        lock.release();bool ok=rpc(socket,"{\"action\":\"cancel\"}",&reply);lock.acquire();
        if(!ok){state_=previous;*error="Matter OTA cancellation could not be confirmed";return false;}
    }
    if(discard)clear();else state_="cancelled";return true;
}
bool OtaStore::matterControl(const std::string& action,const std::string& image,const std::string& node,const std::string& socket,std::string* error,unsigned short endpoint){
    Lock lock(&mutex_);expire();
    if(image!=id_||protocol_!="matter"||state_!="ready"){*error="Upload a Matter image before starting";return false;}
    std::string reply;
    std::string request="{\"action\":\""+action+"\",\"path\":\""+path_+"\",\"node\":\""+jsonEscape(node)+"\"}";
    std::ostringstream suffix;suffix<<",\"endpoint\":"<<endpoint<<"}";request.erase(request.size()-1);request+=suffix.str();
    state_="starting";lock.release();bool ok=rpc(socket,request,&reply);lock.acquire();
    state_="ready";
    if(!ok){if(action=="start"){matterSocket_=socket;target_=node;state_="waiting";error_="Start outcome unknown; checking Matter service";}*error="Matter OTA service unavailable";return false;}
    std::string failure;if(jsonStringField(reply,"error",&failure)&&!failure.empty()){*error=failure;return false;}
    long value=0;
    std::string version;if(jsonStringField(reply,"version",&version)&&!version.empty()&&version.size()<=10&&version.find_first_not_of("0123456789")==std::string::npos){errno=0;unsigned long parsed=strtoul(version.c_str(),0,10);if(errno==0)version_=parsed;}
    if(jsonLongField(reply,"manufacturer",&value)&&value>=0)manufacturer_=value;
    if(jsonLongField(reply,"image_type",&value)&&value>=0)type_=value;
    matterSocket_=socket;if(action=="start"){target_=node;state_="waiting";touched_=time(0);}return true;
}
bool OtaStore::zigbee(const std::string& address,unsigned char command,const std::vector<unsigned char>& a,std::vector<unsigned char>* r){Lock lock(&mutex_);expire();
    if(command!=1&&command!=3&&command!=4&&command!=6)return false;
    r->clear();r->push_back(command==1?2:command==6?7:5);
    bool selected=address==target_&&protocol_=="zigbee"&&active();
    unsigned start=1;unsigned minimum=command==1?9:command==3?14:command==4?18:9;
    if(a.size()<minimum){r->push_back(0x80);return true;}
    bool match=selected&&le(&a[start],2)==manufacturer_&&le(&a[start+2],2)==type_;
    unsigned long current=le(&a[start+4],4);
    if(command==1){
        if(match&&state_=="awaiting_confirmation"&&current==version_)state_="complete";
        bool hw=!hardwareRange_||((a[0]&1)&&a.size()>=11&&le(&a[9],2)>=minHardware_&&le(&a[9],2)<=maxHardware_);
        if(!match||!hw||current>=version_){r->push_back(0x98);return true;}
        zigbeeOffered_=true;r->push_back(0);put(r,manufacturer_,2);put(r,type_,2);put(r,version_,4);put(r,size_,4);touched_=time(0);return true;
    }
    if(!match||!zigbeeOffered_||current!=version_){if(command==6)r->clear();else r->push_back(0x95);return true;}
    touched_=time(0);
    if(command==6){if(a[0]!=0){state_="failed";error_="Accessory rejected firmware";r->clear();return true;}if(progress_<size_){state_="failed";error_="Accessory ended an incomplete transfer";r->clear();return true;}put(r,manufacturer_,2);put(r,type_,2);put(r,version_,4);put(r,0,4);put(r,0,4);state_="awaiting_confirmation";return true;}
    unsigned long offset=le(&a[9],4),count=std::min<unsigned long>(a[13],48);
    if(offset>=size_||count==0){r->push_back(0x95);return true;}
    if(command==4){unsigned long page=le(&a[14],2);if(!page){r->push_back(0x80);return true;}count=std::min(count,page);}
    count=std::min(count,size_-offset);unsigned char bytes[48];if(pread(fd_,bytes,count,offset)!=static_cast<ssize_t>(count)){state_="failed";error_="Firmware read failed";r->push_back(0x95);return true;}
    r->push_back(0);put(r,manufacturer_,2);put(r,type_,2);put(r,version_,4);put(r,offset,4);r->push_back(count);r->insert(r->end(),bytes,bytes+count);if(offset<=progress_)progress_=std::max(progress_,offset+count);state_="transferring";return true;
}
