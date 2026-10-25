// Synthetic fixture only. Never reads production configuration or sockets.
#include "gateway.h"
#include "storage.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <unistd.h>
#include <sys/resource.h>
#ifdef DEVICE_BENCH_ALLOCATIONS
namespace { unsigned long allocations=0,allocatedBytes=0,liveBytes=0,peakBytes=0;
union AllocationHeader { size_t bytes; long double alignment; void* pointer; };
}
void* operator new(size_t size) throw(std::bad_alloc) {
 AllocationHeader* h=static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader)+size));
 if(!h)throw std::bad_alloc();h->bytes=size;++allocations;allocatedBytes+=size;liveBytes+=size;if(liveBytes>peakBytes)peakBytes=liveBytes;return h+1;
}
void operator delete(void* p) throw(){if(p){AllocationHeader* h=static_cast<AllocationHeader*>(p)-1;liveBytes-=h->bytes;std::free(h);}}
void* operator new[](size_t n) throw(std::bad_alloc){return ::operator new(n);}
void operator delete[](void* p) throw(){::operator delete(p);}
#endif
#ifdef DEVICE_COMPONENTS
#define W(d,f) d.WRITE_##f
#define ENABLE(d,component,old) d.component.ensure()
#define T(d) d.thermostat.ensure()
#define WRITE_networkAddress zigbee.ensure().networkAddress
#define WRITE_temperatureCelsius temperature.ensure().celsius
#define WRITE_temperatureKnown temperature.ensure().known
#define WRITE_humidityPercent humidity.ensure().percent
#define WRITE_humidityKnown humidity.ensure().known
#define WRITE_batteryPercent battery.ensure().percent
#define WRITE_powerWatts power.ensure().watts
#define WRITE_powerKnown power.ensure().known
#define WRITE_actorOn actor.ensure().on
#define WRITE_actorKnown actor.ensure().known
#define WRITE_endpoints zigbee.ensure().endpoints
#else
#define W(d,f) d.f
#define ENABLE(d,component,old) d.old=true
#define T(d) d.thermostat
#endif
namespace {
volatile unsigned long sink=0;
double seconds(clockid_t id){timespec t;if(clock_gettime(id,&t))std::abort();return t.tv_sec+t.tv_nsec/1e9;}
void fixture(Device& d,unsigned kind){
 d.ieeeAddress="synthetic-device";d.name="Synthetic fixture";
 if(!kind)return;
 W(d,networkAddress)=7;
 ENABLE(d,temperature,hasTemperature);W(d,temperatureKnown)=true;W(d,temperatureCelsius)=21.5;
 ENABLE(d,battery,hasBattery);W(d,batteryPercent)=80;
 if(kind==1){ENABLE(d,humidity,hasHumidity);W(d,humidityKnown)=true;W(d,humidityPercent)=55;}
 if(kind>=2){
  d.modelId="TRVZB";d.source="zigbee";EndpointDescriptor ep;ep.endpoint=1;ep.inputClusters.push_back(513);W(d,endpoints).push_back(ep);
  unsigned long count;const ThermostatAttribute* fields=thermostatAttributes(&count);
  for(unsigned i=0;i<count;++i)T(d).values[fields[i].id]=20;
  T(d).schedules[0]="00:00 20";T(d).pending.insert(ThermostatField::system_mode);
 }
 if(kind==3){ENABLE(d,humidity,hasHumidity);W(d,humidityKnown)=true;W(d,humidityPercent)=55;ENABLE(d,power,hasPower);W(d,powerKnown)=true;W(d,powerWatts)=12;ENABLE(d,actor,hasActor);W(d,actorKnown)=true;W(d,actorOn)=true;}
}
}
int main(int argc,char** argv){
 if(argc!=4)return 2;const std::string operation=argv[1];unsigned kind=std::strtoul(argv[2],0,10),iterations=std::strtoul(argv[3],0,10);
 if(kind>4||!iterations||iterations>1000000)return 2;setpriority(PRIO_PROCESS,0,19);
 std::vector<Device> devices(kind==4?26:1);
 for(unsigned i=0;i<devices.size();++i){fixture(devices[i],kind==4?i%4:kind);char id[40];std::sprintf(id,"synthetic-%u",i);devices[i].ieeeAddress=id;if(kind==4&&i%4)W(devices[i],networkAddress)=i+1;}
#ifdef DEVICE_BENCH_ALLOCATIONS
 unsigned long fixtureBytes=liveBytes,fixtureAllocations=allocations;
#endif
 std::vector<Automation::SnapshotPtr> previous(devices.size());
 for(unsigned i=0;i<devices.size();++i)previous[i]=Automation::capture(devices[i]);
 char directory[]="/tmp/microhad-components-XXXXXX";if(!mkdtemp(directory))return 2;std::string path=std::string(directory)+"/synthetic.pb",error;
#ifdef DEVICE_BENCH_ALLOCATIONS
 allocations=allocatedBytes=0;peakBytes=liveBytes;
#endif
 double cpu=seconds(CLOCK_PROCESS_CPUTIME_ID),wall=seconds(CLOCK_MONOTONIC);
 for(unsigned n=0;n<iterations;++n){
  if(operation=="construct"){Device d;sink+=d.name.size();}
  else if(operation=="copy"){std::vector<Device> copy(devices);sink+=copy.size();}
  else if(operation=="snapshot"||operation=="unchanged"){
   for(unsigned i=0;i<devices.size();++i){if(operation=="snapshot"){devices[i].lastSeen=n+1;if(kind==2||kind==3||(kind==4&&i%4>=2))T(devices[i]).values[ThermostatField::occupied_heating_setpoint]=21+(n%2);}
    Automation::SnapshotPtr next=Automation::capture(devices[i],previous[i]);sink+=next->present;previous[i]=next;}
  }else if(operation=="capabilities"){
   unsigned long count;const ThermostatAttribute* fields=thermostatAttributes(&count);
   for(unsigned i=0;i<count;++i){ThermostatAttribute a=thermostatDefinition(devices[0],fields[i]);sink+=thermostatSupported(devices[0],fields[i])+static_cast<unsigned>(a.maximum+300);}
  }else if(operation=="cache"){if(!saveDeviceCache(path,devices,&error))return 3;}
  else return 2;
 }
 cpu=seconds(CLOCK_PROCESS_CPUTIME_ID)-cpu;wall=seconds(CLOCK_MONOTONIC)-wall;
 std::printf("{\"cpu_us\":%.6f,\"wall_us\":%.6f,\"inline_bytes\":%lu,\"iterations\":%u",cpu*1e6/iterations,wall*1e6/iterations,static_cast<unsigned long>(sizeof(Device)),iterations);
#ifdef DEVICE_BENCH_ALLOCATIONS
 std::printf(",\"allocations\":%lu,\"allocated_bytes\":%lu,\"fixture_allocations\":%lu,\"fixture_bytes\":%lu,\"peak_bytes\":%lu",allocations,allocatedBytes,fixtureAllocations,fixtureBytes,peakBytes);
#endif
 std::puts("}");
 if(operation=="cache"){std::vector<Device> loaded;if(!loadDeviceCache(path,&loaded,&error)||loaded.size()!=devices.size())return 4;}
 unlink(path.c_str());rmdir(directory);return 0;
}
