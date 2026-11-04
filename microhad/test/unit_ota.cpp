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
void runOtaGatewayPageTests();
namespace {
void put(std::string* s,unsigned long n,unsigned bytes){for(unsigned i=0;i<bytes;++i)s->push_back(static_cast<char>(n>>(i*8)));}
std::string hex(const std::string& s){const char* digits="0123456789abcdef";std::string out;for(unsigned long i=0;i<s.size();++i){unsigned char b=s[i];out+=digits[b>>4];out+=digits[b&15];}return out;}
std::vector<unsigned char> args(unsigned long version){std::string s;s.push_back(0);put(&s,0x1234,2);put(&s,0x5678,2);put(&s,version,4);return std::vector<unsigned char>(s.begin(),s.end());}
std::string upload(const std::string& bytes){OtaStore& ota=OtaStore::instance();std::string error,id;std::ostringstream b;b<<"{\"size\":"<<bytes.size()<<",\"protocol\":\"zigbee\"}";assert(ota.upload("begin",b.str(),&error));assert(jsonStringField(ota.status(),"image",&id));std::string prefix="{\"image\":\""+id+"\"";
assert(!ota.upload("chunk",prefix+",\"offset\":1,\"data\":\"00\"}",&error));
assert(ota.upload("chunk",prefix+",\"offset\":0,\"data\":\""+hex(bytes)+"\"}",&error));assert(!ota.upload("chunk",prefix+",\"offset\":0,\"data\":\"00\"}",&error));assert(ota.upload("finish",prefix+"}",&error));return id;}
}
namespace {
extern "C" int microhad_catalog_public_ipv4(unsigned long);
class CatalogTransport : public HttpsCatalogHook {
public:
    int calls; std::string reply;
    CatalogTransport():calls(0),reply("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]"){}
    bool get(const std::string& host,const std::string& path,const std::string&,std::string* response,std::string*) {
        assert(host=="raw.githubusercontent.com" && path=="/Koenkk/zigbee-OTA/master/index.json");
        ++calls;*response=reply;return true;
    }
};
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
    assert(fetchFirmwareCatalog("zigbee-ota",&body,&error)&&hook.calls==1);
    configureFirmwareCatalogs("","test-ca");hook.reply="HTTP/1.1 302 Found\r\nLocation: https://127.0.0.1/private\r\n\r\n";
    assert(!fetchFirmwareCatalog("zigbee-ota",&body,&error));assert(error.find("not configured")!=std::string::npos);
    configureFirmwareCatalogs("","test-ca");hook.reply="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n[]\r\n0\r\n\r\n";
    assert(fetchFirmwareCatalog("zigbee-ota",&body,&error)&&body=="[]");
    configureFirmwareCatalogs("","test-ca");hook.reply="HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\n[]";
    assert(!fetchFirmwareCatalog("zigbee-ota",&body,&error));
    configureFirmwareCatalogs("","test-ca");hook.reply="HTTP/1.1 200 OK\r\n\r\n"+std::string(4*1024*1024+1,'[');
    assert(!fetchFirmwareCatalog("zigbee-ota",&body,&error));
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
    catalogTests(); otaIdentityTests();
    OtaStore& ota=OtaStore::instance();std::string error;
    assert(!ota.upload("begin","{\"size\":16777217,\"protocol\":\"zigbee\"}",&error));
    std::string image;put(&image,0x0beef11e,4);put(&image,0x100,2);put(&image,56,2);put(&image,0,2);put(&image,0x1234,2);put(&image,0x5678,2);put(&image,2,4);put(&image,2,2);image.append(32,'\0');put(&image,162,4);put(&image,0,2);put(&image,100,4);image.append(100,'x');assert(image.size()==162);
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
