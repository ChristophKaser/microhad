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
void runOtaUnitTests(){
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
