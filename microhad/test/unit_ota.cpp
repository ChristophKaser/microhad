#include "firmware_catalog.h"
#include "https_client.h"
#include "gateway.h"
#include "zcl.h"
#include "storage.h"
#include <fstream>
#include <unistd.h>
#include "ota.h"
#include "json_util.h"
#include <cassert>
#include <cstdio>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <cstring>
bool testMatterOtaRpc(const std::string& socket);
void runOtaGatewayPageTests();
void runOtaNotificationProfileTest(unsigned short profile, const std::string& image);

namespace {
void otaSocketProtocol(){
    std::ostringstream name;name<<"/tmp/microhad-ota-wire-"<<getpid();const std::string path=name.str();
    int fd=socket(AF_UNIX,SOCK_STREAM,0);assert(fd>=0);sockaddr_un address;std::memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;std::strcpy(address.sun_path,path.c_str());
    assert(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);assert(listen(fd,1)==0);
    pid_t child=fork();assert(child>=0);
    if(child==0){int peer=accept(fd,0,0);if(peer<0)_exit(1);unsigned char request[16];size_t n=0;while(n<sizeof(request)){ssize_t got=read(peer,request+n,sizeof(request)-n);if(got<=0)_exit(2);n+=got;}
        const unsigned char expected[]={14,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0};if(std::memcmp(request,expected,sizeof(expected)))_exit(3);
        const unsigned char reply[]={25,0,1,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,'e','m','p','t','y'};
        if(write(peer,reply,3)!=3||write(peer,reply+3,sizeof(reply)-3)!=static_cast<ssize_t>(sizeof(reply)-3))_exit(4);
        close(peer);close(fd);_exit(0);
    }
    assert(testMatterOtaRpc(path));int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);close(fd);assert(unlink(path.c_str())==0);
}
void put(std::string* s,unsigned long n,unsigned bytes){for(unsigned i=0;i<bytes;++i)s->push_back(static_cast<char>(n>>(i*8)));}
std::string hex(const std::string& s){const char* digits="0123456789abcdef";std::string out;for(unsigned long i=0;i<s.size();++i){unsigned char b=s[i];out+=digits[b>>4];out+=digits[b&15];}return out;}
std::vector<unsigned char> args(unsigned long version){std::string s;s.push_back(0);put(&s,0x1234,2);put(&s,0x5678,2);put(&s,version,4);return std::vector<unsigned char>(s.begin(),s.end());}
std::string upload(const std::string& bytes){OtaStore& ota=OtaStore::instance();std::string error,id;std::ostringstream b;b<<"{\"size\":"<<bytes.size()<<",\"protocol\":\"zigbee\"}";assert(ota.upload("begin",b.str(),&error));assert(jsonStringField(ota.status(),"image",&id));std::string prefix="{\"image\":\""+id+"\"";
assert(!ota.upload("chunk",prefix+",\"offset\":1,\"data\":\"00\"}",&error));
assert(ota.upload("chunk",prefix+",\"offset\":0,\"data\":\""+hex(bytes)+"\"}",&error));assert(!ota.upload("chunk",prefix+",\"offset\":0,\"data\":\"00\"}",&error));assert(ota.upload("finish",prefix+"}",&error));return id;}
}
namespace {
extern "C" int microhad_catalog_public_ipv4(unsigned long);
static unsigned catalogDelivered = 0;
class CatalogTransport : public HttpsCatalogHook {
public:
    int calls; bool observedStreaming; std::string reply;
    CatalogTransport():calls(0),observedStreaming(false),reply("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]"){}
    bool stream(const std::string& host,const std::string& path,const std::string&,HttpsCatalogRawSink* sink,std::string*) {
        assert((host=="raw.githubusercontent.com" && path=="/Koenkk/zigbee-OTA/master/index.json") || (host=="on.test-net.dcl.csa-iot.org" && path=="/dcl/model/versions/4476/4096/17039372"));
        ++calls;
        for (unsigned long i=0;i<reply.size();++i) {
            if(!sink->write(reply.data()+i,1)) return false;
            if (catalogDelivered && i+1<reply.size()) observedStreaming=true;
        }
        return true;
    }
};
class CollectCatalog : public FirmwareCatalogSink {
public:
    std::string body; unsigned writes; bool failWrites;
    CollectCatalog():writes(0),failWrites(false) {}
    bool begin() { return true; }
    bool write(const char* data,unsigned long size) { ++writes; ++catalogDelivered; if(failWrites) return false; body.append(data,size); return true; }
};
class CountCatalog : public FirmwareCatalogSink {
public:
    unsigned long bytes, writes;
    CountCatalog():bytes(0),writes(0) {}
    bool begin() { return true; }
    bool write(const char*,unsigned long size) { bytes+=size; ++writes; return true; }
};
class OversizedCatalog : public HttpsCatalogHook {
public:
    bool stream(const std::string&,const std::string&,const std::string&,HttpsCatalogRawSink* sink,std::string*) {
        const char* header="HTTP/1.1 200 OK\r\n\r\n[";
        if(!sink->write(header,std::strlen(header))) return false;
        char block[1024]; std::memset(block,' ',sizeof(block));
        for (unsigned i=0;i<4097;++i) if(!sink->write(block,sizeof(block))) return false;
        return true;
    }
};
bool fetchFirmwareCatalog(const std::string& id, std::string* body, std::string* error) {
    CollectCatalog sink; bool started=false;
    const bool ok=streamFirmwareCatalog(id,&sink,&started,error); *body=sink.body; return ok;
}
void catalogTests() {
    const std::string url="https://raw.githubusercontent.com/Koenkk/zigbee-OTA/master/index.json";
    assert(firmwareCatalogDestination(url,url));
    const char* denied[]={"http://raw.githubusercontent.com/Koenkk/zigbee-OTA/master/index.json",
        "https://raw.githubusercontent.com.evil.test/Koenkk/zigbee-OTA/master/index.json",
        "https://raw.githubusercontent.com/other", "https://127.0.0.1/", "https://user@raw.githubusercontent.com/", "https://raw.githubusercontent.com/%2e%2e/"};
    for(unsigned i=0;i<sizeof(denied)/sizeof(*denied);++i) assert(!firmwareCatalogDestination(url,denied[i]));
    assert(microhad_catalog_public_ipv4(0x08080808));
    const unsigned long addresses[]={0,0x0a000001,0x7f000001,0xa9fe0001,0xac100001,0xc0a80001,0x64400001,0xe0000001,0xc0000201};
    for(unsigned i=0;i<sizeof(addresses)/sizeof(*addresses);++i)assert(!microhad_catalog_public_ipv4(addresses[i]));
    CatalogTransport hook;setHttpsCatalogHook(&hook);std::string body,error;
    configureFirmwareCatalogs("","test-ca");
    assert(firmwareCatalogsJson().find("sonoff")!=std::string::npos);
    assert(!fetchFirmwareCatalog("https://example.com",&body,&error));assert(hook.calls==0);
    assert(fetchFirmwareCatalog("zigbee-ota",&body,&error)&&body=="[]");
    assert(fetchFirmwareCatalog("zigbee-ota",&body,&error)&&hook.calls==2);
    configureFirmwareCatalogs("","test-ca");hook.reply="HTTP/1.1 302 Found\r\nLocation: https://127.0.0.1/private\r\n\r\n";
    assert(!fetchFirmwareCatalog("zigbee-ota",&body,&error));assert(error.find("not configured")!=std::string::npos);
    configureFirmwareCatalogs("","test-ca");hook.reply="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n[]\r\n0\r\n\r\n";
    assert(fetchFirmwareCatalog("zigbee-ota",&body,&error)&&body=="[]");
    configureFirmwareCatalogs("","test-ca"); hook.reply="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\n[\r\n1\r\n]\r\n0\r\nX-Test: yes\r\n\r\n";
    catalogDelivered=0; hook.observedStreaming=false;
    assert(fetchFirmwareCatalog("zigbee-ota",&body,&error)&&body=="[]"&&hook.observedStreaming);
    CollectCatalog disconnected; disconnected.failWrites=true; bool started=false;
    assert(!streamFirmwareCatalog("zigbee-ota",&disconnected,&started,&error) && started && disconnected.writes==1);
    configureFirmwareCatalogs("","test-ca");hook.reply="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n[]\r\n";
    assert(!fetchFirmwareCatalog("zigbee-ota",&body,&error));
    configureFirmwareCatalogs("","test-ca");hook.reply="HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n";
    assert(!fetchFirmwareCatalog("zigbee-ota",&body,&error));
    configureFirmwareCatalogs("","test-ca");hook.reply="HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\n[]";
    assert(!fetchFirmwareCatalog("zigbee-ota",&body,&error));
    configureFirmwareCatalogs("","test-ca");hook.reply="HTTP/1.1 200 OK\r\nContent-Length: 4194305\r\n\r\n";
    assert(!fetchFirmwareCatalog("zigbee-ota",&body,&error));
    configureFirmwareCatalogs("","test-ca");hook.reply="HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
    assert(fetchFirmwareCatalog("matter-dcl-testnet/4476/4096/17039372",&body,&error)&&body=="{}");
    configureFirmwareCatalogs("","test-ca");hook.reply="HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    assert(fetchFirmwareCatalog("matter-dcl-testnet/4476/4096/17039372",&body,&error)&&body=="{\"notFound\":true}");
    const char* badDcl[]={"matter-dcl-testnet/4476/4096/../1", "matter-dcl-testnet/4476/4096/1/2", "matter-dcl-testnet/65536/4096", "matter-dcl-testnet/4476/4096/4294967296", "matter-dcl-testnet/4476/4096/", "matter-dcl-testnet/04476/4096"};
    for(unsigned i=0;i<sizeof(badDcl)/sizeof(*badDcl);++i) assert(!fetchFirmwareCatalog(badDcl[i],&body,&error));
    OversizedCatalog oversized; setHttpsCatalogHook(&oversized); CountCatalog bounded; bool bodyStarted=false;
    assert(!streamFirmwareCatalog("zigbee-ota",&bounded,&bodyStarted,&error));
    assert(bodyStarted && bounded.bytes <= 4UL*1024*1024 && bounded.writes > 1000);
    configureFirmwareCatalogs("/tmp/nonexistent-microhad-catalog-config","");assert(firmwareCatalogsJson()=="null");
    configureFirmwareCatalogs("","test-ca");setHttpsCatalogHook(0);
}
void otaIdentityTests() {
    Device device;device.ieeeAddress="0x00124b0000000001";
    assert(device.zigbee.read().otaVersion==-1);
    const unsigned char raw[]={2,0,0,0x23,0xff,0xff,0xff,0xff,7,0,0,0x21,0x86,0x12,8,0,0,0x21,0x01,0x20};
    std::vector<unsigned char> records(raw,raw+sizeof(raw));
    assert(applyZclReadAttributeResponse(&device,0x19,records,0,0));
    assert(device.zigbee.read().otaVersion==4294967295LL && device.zigbee.read().otaManufacturer==4742 && device.zigbee.read().otaImageType==8193);
    records[3]=0x21;assert(!applyZclReadAttributeResponse(&device,0x19,records,0,0));
    const std::string path="/tmp/microhad-ota-identity-test-"+device.ieeeAddress;
    std::vector<Device> saved(1,device),loaded;std::string error;
    assert(saveDeviceCache(path,saved,&error));assert(loadDeviceCache(path,&loaded,&error));
    assert(loaded.size()==1 && loaded[0].zigbee.read().otaVersion==4294967295LL && loaded[0].zigbee.read().otaImageType==8193);
    unlink(path.c_str());
}
}
void runOtaUnitTests(){
    otaSocketProtocol(); catalogTests(); otaIdentityTests();
    OtaStore& ota=OtaStore::instance();std::string error;
    assert(!ota.upload("begin","{\"size\":16777217,\"protocol\":\"zigbee\"}",&error));
    std::string image;put(&image,0x0beef11e,4);put(&image,0x100,2);put(&image,56,2);put(&image,0,2);put(&image,0x1234,2);put(&image,0x5678,2);put(&image,2,4);put(&image,2,2);image.append(32,'\0');put(&image,162,4);put(&image,0,2);put(&image,100,4);image.append(100,'x');assert(image.size()==162);
    for (unsigned i=0;i<2;++i) {
        std::string selected=upload(image);
        runOtaNotificationProfileTest(i==0 ? 0xc05e : 0x0104, selected);
        assert(ota.cancel(selected,true,&error));
    }
    std::string id=upload(image);assert(!ota.startZigbee("stale","device",&error));assert(ota.startZigbee(id,"device",&error));assert(!ota.startZigbee(id,"other",&error));assert(!ota.upload("begin","{\"size\":162,\"protocol\":\"zigbee\"}",&error));
    std::vector<unsigned char> r,q=args(1);assert(ota.zigbee("other",1,q,&r)&&r.size()==2&&r[1]==0x98);assert(ota.zigbee("device",1,q,&r)&&r.size()==14&&r[1]==0);
    q=args(2);std::string offset;put(&offset,160,4);q.insert(q.end(),offset.begin(),offset.end());q.push_back(48);assert(ota.zigbee("device",3,q,&r)&&r[1]==0&&r.back()=='x');
    long progress=-1;assert(jsonLongField(ota.status(),"progress",&progress)&&progress==0); // A skipped range cannot complete an update.
    for(unsigned long at=0;at<162;at+=48){q=args(2);offset.clear();put(&offset,at,4);q.insert(q.end(),offset.begin(),offset.end());q.push_back(48);assert(ota.zigbee("device",3,q,&r)&&r[1]==0);assert(r[14]==(at+48<=162?48:162-at));}
    runOtaGatewayPageTests();
    q=args(2);assert(ota.zigbee("device",6,q,&r)&&r.size()==17);assert(ota.status().find("awaiting_confirmation")!=std::string::npos);
    assert(ota.zigbee("device",1,q,&r)&&r[1]==0x98);assert(ota.status().find("complete")!=std::string::npos);
    assert(ota.cancel(id,true,&error));assert(ota.status().find("empty")!=std::string::npos);
    id=upload(image);assert(ota.startZigbee(id,"device",&error));assert(ota.cancel(id,false,&error));q=args(1);assert(ota.zigbee("device",1,q,&r)&&r[1]==0x98);assert(ota.cancel(id,true,&error));
    std::ostringstream begin;begin<<"{\"size\":162,\"protocol\":\"zigbee\"}";assert(ota.upload("begin",begin.str(),&error));assert(jsonStringField(ota.status(),"image",&id));assert(!ota.upload("finish","{\"image\":\""+id+"\"}",&error));assert(ota.cancel(id,true,&error));
    puts("unit: bounded temporary OTA uploads, selection, contiguous blocks, confirmation and cancellation");
}
