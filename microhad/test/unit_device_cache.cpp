#include "gateway.h"
#include "storage.h"
#include "sensor_history.h"
#include "cache_fixture.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
extern void (*deviceCacheWriteHook)(void*);
extern void* deviceCacheWriteHookData;
namespace {
std::string readBytes(const std::string& path) {
    std::ifstream in(path.c_str(),std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
void writeBytes(const std::string& path,const std::string& bytes) {
    std::ofstream out(path.c_str(),std::ios::binary);out.write(bytes.data(),bytes.size());assert(out.good());
}
struct Barrier {
    pthread_mutex_t mutex;pthread_cond_t ready;bool entered,released;
    Barrier():entered(false),released(false){pthread_mutex_init(&mutex,0);pthread_cond_init(&ready,0);}
    ~Barrier(){pthread_cond_destroy(&ready);pthread_mutex_destroy(&mutex);}
    static void block(void* ptr){Barrier* b=static_cast<Barrier*>(ptr);pthread_mutex_lock(&b->mutex);b->entered=true;pthread_cond_broadcast(&b->ready);while(!b->released)pthread_cond_wait(&b->ready,&b->mutex);pthread_mutex_unlock(&b->mutex);}
    void wait(){pthread_mutex_lock(&mutex);while(!entered)pthread_cond_wait(&ready,&mutex);pthread_mutex_unlock(&mutex);}
    void release(){pthread_mutex_lock(&mutex);released=true;pthread_cond_broadcast(&ready);pthread_mutex_unlock(&mutex);}
};
void codec(const std::string& root) {
    const std::string cache=root+"/cache.pb";std::string error;
    std::vector<Device> devices(4),loaded;
    for(unsigned i=0;i<4;++i){devices[i].ieeeAddress=std::string("id-")+static_cast<char>('0'+i);devices[i].name="Büro \"lamp\"\n";}
    Device& d=devices[0];d.networkAddress=0x1234;d.hasTemperature=true;d.temperatureKnown=true;d.temperatureCelsius=-12.375;
    d.hasLight=true;d.lightKnown=true;d.lightOn=true;d.hasBrightness=true;d.brightnessKnown=true;d.brightnessPercent=55;
    d.thermostat.sourceId="remote";d.thermostat.staleMinutes=60;d.lastSeen=1700000123;
    EndpointDescriptor endpoint;endpoint.endpoint=1;endpoint.profileId=260;endpoint.inputClusters.push_back(6);d.endpoints.push_back(endpoint);
    d.initialInterviewComplete=true;d.nodeDescriptor.assign(13,0x42);
    DeviceValueAlert alert;alert.hasHigh=true;alert.high=30.5;alert.hysteresis=1.25;alert.unit="°C";d.valueAlerts["temperature"]=alert;
    devices[1].source="matter";devices[1].matterNode="0000000000000012";devices[1].matterEndpoint=2;
    devices[1].matterServers.push_back(6);devices[1].matterClients.push_back(8);devices[1].matterDeviceTypes.push_back(256);
    devices[1].matterState="complete";devices[1].matterMetadataState="stale";
    devices[2].source="shelly";devices[2].externalId="uni";devices[2].component="relay:0";devices[2].hasActor=true;devices[2].actorKnown=true;devices[2].actorOn=true;
    devices[3].source="fritz_box";devices[3].hasWifiSignal=true;devices[3].wifiSignalPercent=70;
    assert(saveDeviceCache(cache,devices,&error));assert(loadDeviceCache(cache,&loaded,&error));assert(loaded.size()==4);
    assert(loaded[0].lightKnown&&loaded[0].brightnessKnown&&loaded[0].temperatureCelsius==-12.375);
    assert(saveDevices(root+"/before.json",devices,&error));assert(saveDevices(root+"/after.json",loaded,&error));
    assert(readBytes(root+"/before.json")==readBytes(root+"/after.json"));
    struct stat st;assert(stat(cache.c_str(),&st)==0&&(st.st_mode&0777)==0600);
    const std::string valid=readBytes(cache);std::string bad=valid;bad[1]=2;writeBytes(cache,bad);
    loaded.resize(1);loaded[0].ieeeAddress="sentinel";
    assert(!loadDeviceCache(cache,&loaded,&error)&&loaded[0].ieeeAddress=="sentinel");
    writeBytes(cache,valid.substr(0,valid.size()-1));assert(!loadDeviceCache(cache,&loaded,&error));
    writeBytes(cache,valid+"x");assert(!loadDeviceCache(cache,&loaded,&error));
    bad=valid;bad.append("\x08\x01",2);writeBytes(cache,bad);assert(!loadDeviceCache(cache,&loaded,&error));
    writeBytes(cache,valid);devices[0].temperatureCelsius=std::numeric_limits<double>::quiet_NaN();
    assert(!saveDeviceCache(cache,devices,&error)&&readBytes(cache)==valid);
    devices[0].temperatureCelsius=0;devices[0].name.assign(65537,'x');
    assert(!saveDeviceCache(cache,devices,&error)&&readBytes(cache)==valid);
    assert(!saveDeviceCache(root,loaded,&error));
    unlink(cache.c_str());assert(symlink((root+"/before.json").c_str(),cache.c_str())==0);assert(!loadDeviceCache(cache,&loaded,&error));unlink(cache.c_str());
    unlink((root+"/before.json").c_str());unlink((root+"/after.json").c_str());
}
void worker(const std::string& root) {
    const std::string cache=root+"/worker.pb",legacy=root+"/legacy.json";std::string error;
    writeBytes(legacy,"legacy");Barrier barrier;
    {
        // No flash path and no history initialization: cache persistence still works.
        SensorHistory history(root+"/unused");
        deviceCacheWriteHook=Barrier::block;deviceCacheWriteHookData=&barrier;
        std::vector<Device> first(1);first[0].ieeeAddress="old";
        assert(history.queueDeviceCache(cache,&first,legacy,&error));barrier.wait();
        assert(access(cache.c_str(),F_OK)!=0); // Writer is stalled, but submissions remain available.
        std::vector<Device> middle(1);middle[0].ieeeAddress="middle";assert(history.queueDeviceCache(cache,&middle,legacy,&error));
        std::vector<Device> last(1);last[0].ieeeAddress="latest";assert(history.queueDeviceCache(cache,&last,legacy,&error));
        barrier.release();assert(history.flushDeviceCache(&error));
        std::vector<Device> loaded;assert(loadDeviceCache(cache,&loaded,&error)&&loaded.size()==1&&loaded[0].ieeeAddress=="latest");
        assert(access(legacy.c_str(),F_OK)!=0);
    }
    deviceCacheWriteHook=0;deviceCacheWriteHookData=0;
    unlink(cache.c_str());
    {
        SensorHistory history(root+"/unused",root+"/bad-flash");writeBytes(root+"/bad-flash","invalid header");assert(history.initialize(&error));
        std::vector<Device> snapshot(1);snapshot[0].ieeeAddress="retry";
        const std::string folder=root+"/missing";
        assert(history.queueDeviceCache(folder+"/cache.pb",&snapshot,"",&error));
        assert(!history.flushDeviceCache(&error));assert(!history.flushDeviceCache(&error));
        assert(mkdir(folder.c_str(),0700)==0);assert(history.flushDeviceCache(&error));
        std::vector<Device> loaded;assert(loadDeviceCache(folder+"/cache.pb",&loaded,&error)&&loaded[0].ieeeAddress=="retry");
        unlink((folder+"/cache.pb").c_str());rmdir(folder.c_str());
    }
    unlink((root+"/bad-flash").c_str());unlink((root+"/unused.values").c_str());
}
}
void runDeviceCacheUnitTests() {
    puts("unit: direct protobuf cache, owned latest snapshot, background write and retry");
    char folder[]="/tmp/microhad-protobuf-cache-XXXXXX";assert(mkdtemp(folder));
    codec(folder);worker(folder);assert(rmdir(folder)==0);
}
