#include "gateway.h"
#include "gateway_runner.h"
#include "automation_types.h"
#include "thermostat.h"
#include <ctime>
#include <cstdio>
#include <cstdlib>
// Use the existing test peer to benchmark the real published-view accessor.
class AutomationRunnerTestPeer {
public:
    static void benchmarkStatus(GatewayView& view,volatile unsigned long* consumed) {
        GatewayRunner::ViewData published(&view);
        const std::clock_t start=std::clock();
        for(int i=0;i<3000;++i){
#ifdef CACHED_STATUS
            *consumed+=published.statusJson().size();
#else
            *consumed+=published.view->statusJson().size();
#endif
        }
        std::printf("published_status_cpu_us %.6f\n",1e6*double(std::clock()-start)/CLOCKS_PER_SEC/3000);
    }
};
int main(){
 Device d;d.ieeeAddress="synthetic";d.name="Synthetic thermostat";d.source="zigbee";
 EndpointDescriptor ep;ep.endpoint=1;ep.inputClusters.push_back(513);d.zigbee.ensure().endpoints.push_back(ep);
 unsigned long n=0;const ThermostatAttribute* attrs=thermostatAttributes(&n);
 for(unsigned long i=0;i<n;++i)d.thermostat.read().values[
#ifdef TYPED_THERMOSTAT
 attrs[i].id
#else
 attrs[i].name
#endif
 ]=20;
 volatile double checked=0;
 std::clock_t checksStart=std::clock();
 for(int repeat=0;repeat<30000;++repeat)for(unsigned long i=0;i<n;++i){
  ThermostatAttribute definition=thermostatDefinition(d,attrs[i]);
  checked+=definition.minimum+definition.maximum+thermostatSupported(d,attrs[i]);
 }
 std::printf("thermostat_capability_sweep_cpu_us %.6f\n",1e6*double(std::clock()-checksStart)/CLOCKS_PER_SEC/30000);
 std::printf("thermostat_state_inline_bytes %lu\n",static_cast<unsigned long>(sizeof(ThermostatState)));
 Automation::SnapshotPtr previous=Automation::capture(d);
 volatile unsigned long consumed=0;
 for(int mode=0;mode<2;++mode){std::clock_t start=std::clock();
 for(int i=0;i<3000;++i){if(mode)d.thermostat.read().values[
#ifdef TYPED_THERMOSTAT
 ThermostatField::occupied_heating_setpoint
#else
 "occupied_heating_setpoint"
#endif
 ]=20+(i%2);Automation::SnapshotPtr next=Automation::capture(d,previous);consumed+=next->present;previous=next;}
 std::printf("thermostat_%s_cpu_us %.6f\n",mode?"changed":"unchanged",1e6*double(std::clock()-start)/CLOCKS_PER_SEC/3000);
 }
 GatewayView view;
 for(int i=0;i<26;++i){Device device;char id[80];std::sprintf(id,"matter:00000000000000000000000000000000:%04d",i);device.ieeeAddress=id;device.source="matter";device.matter.ensure().endpoint=i+1;device.temperature.setPresent(device.temperature.ensure().known=true);device.temperature.ensure().celsius=21.125;view.devices.push_back(device);}
 std::clock_t start=std::clock();for(int i=0;i<3000;++i)consumed+=view.statusJson().size();
 std::printf("status_26_endpoints_cpu_us %.6f\n",1e6*double(std::clock()-start)/CLOCKS_PER_SEC/3000);
 AutomationRunnerTestPeer::benchmarkStatus(view,&consumed);
 return consumed==0;
}
