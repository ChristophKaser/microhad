#include "gateway.h"
#include "automation_types.h"
#include "thermostat.h"
#include <ctime>
#include <cstdio>
#include <cstdlib>
int main(){
 Device d;d.ieeeAddress="synthetic";d.name="Synthetic thermostat";d.source="zigbee";
 EndpointDescriptor ep;ep.endpoint=1;ep.inputClusters.push_back(513);d.endpoints.push_back(ep);
 unsigned long n=0;const ThermostatAttribute* attrs=thermostatAttributes(&n);
 for(unsigned long i=0;i<n;++i)d.thermostat.values[attrs[i].name]=20;
 Automation::SnapshotPtr previous=Automation::capture(d);
 volatile unsigned long consumed=0;
 for(int mode=0;mode<2;++mode){std::clock_t start=std::clock();
 for(int i=0;i<3000;++i){if(mode)d.thermostat.values["occupied_heating_setpoint"]=20+(i%2);Automation::SnapshotPtr next=Automation::capture(d,previous);consumed+=next->present;previous=next;}
 std::printf("thermostat_%s_cpu_us %.6f\n",mode?"changed":"unchanged",1e6*double(std::clock()-start)/CLOCKS_PER_SEC/3000);
 }
 GatewayView view;
 for(int i=0;i<26;++i){Device device;char id[80];std::sprintf(id,"matter:00000000000000000000000000000000:%04d",i);device.ieeeAddress=id;device.source="matter";device.matterEndpoint=i+1;device.hasTemperature=device.temperatureKnown=true;device.temperatureCelsius=21.125;view.devices.push_back(device);}
 std::clock_t start=std::clock();for(int i=0;i<3000;++i)consumed+=view.statusJson().size();
 std::printf("status_26_endpoints_cpu_us %.6f\n",1e6*double(std::clock()-start)/CLOCKS_PER_SEC/3000);
 return consumed==0;
}
