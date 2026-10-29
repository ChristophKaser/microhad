#include "cache_fixture.h"
#include "ash.h"
#include "gateway.h"
#if MICROHAD_ZIGGURAT_ENABLED
#include "ziggurat_client.h"
#endif
#include <sys/socket.h>
#include "storage.h"
#include "zcl.h"
#include "matter_client.h"
#include "fcm.h"
#include "notification_worker.h"
#include "json_util.h"
#include "json_writer.h"
#include "proc_stats.h"
#include "web_push.h"

#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

class GatewayTestPeer {
public:

    static void deviceCacheBatching() {
        char directory[]="/tmp/microhad-cache-batch-XXXXXX";assert(mkdtemp(directory));
        const std::string cache="/tmp/microhad-values.pb";
        std::string error;Device device;device.ieeeAddress="cache-lamp";device.light.setPresent(true);device.light.ensure().known=true;
        std::vector<Device> saved(1,device);assert(saveDevices(std::string(directory)+"/devices.json",saved,&error));
        {
            Gateway gateway("/dev/null",115200,directory,"","","");
            gateway.history_.useManualPersistenceForTests();gateway.devices_[device.ieeeAddress]=device;
            gateway.saveState();const uint64_t first=gateway.deviceCacheDueMs_;
            assert(gateway.deviceCacheWaitMilliseconds()>4500&&gateway.deviceCacheWaitMilliseconds()<=5000);
            gateway.devices_[device.ieeeAddress].light.ensure().on=true;gateway.saveState();
            assert(gateway.deviceCacheDueMs_==first);
            gateway.flushState(false);assert(access(cache.c_str(),F_OK)!=0);
            gateway.deviceCacheDueMs_=0;gateway.flushState(false);
            assert(!gateway.deviceCacheDirty_&&access(cache.c_str(),F_OK)!=0);
            assert(gateway.history_.flushDeviceCache(&error));
            std::vector<Device> loaded;assert(loadDeviceCache(cache,&loaded,&error)&&loaded[0].light.read().on);
            gateway.devices_[device.ieeeAddress].light.ensure().on=false;gateway.saveState();
            gateway.stop();assert(loadDeviceCache(cache,&loaded,&error)&&!loaded[0].light.read().on);
        }
        saved[0].name="Saved name";saved[0].sensorRefreshIntervalMinutes=17;
        saved[0].contactReminderAlertMinutes=123;
        DeviceValueAlert setting;setting.hasHigh=true;setting.high=30;setting.hysteresis=2;
        saved[0].valueAlerts["temperature"]=setting;
        assert(saveDevices(std::string(directory)+"/devices.json",saved,&error));
        {
            Gateway restored("/dev/null",115200,directory,"","","");restored.loadState();
            const Device& loaded=restored.devices_[device.ieeeAddress];
            assert(loaded.light.read().known&&!loaded.light.read().on&&loaded.name=="Saved name");
            assert(loaded.sensorRefreshIntervalMinutes==17&&loaded.contactReminderAlertMinutes==123);
            assert(loaded.valueAlerts.find("temperature")->second.high==30);
        }
        // A malformed new cache must not resurrect a stale legacy snapshot.
        {std::ofstream bad(cache.c_str());bad<<"bad";}
        saved[0].light.ensure().on=true;assert(saveDevices("/tmp/microhad-values.json",saved,&error));
        {
            Gateway restored("/dev/null",115200,directory,"","","");restored.loadState();
            assert(!restored.devices_[device.ieeeAddress].light.read().on);
        }
        unlink(cache.c_str());
        {
            Gateway legacy("/dev/null",115200,directory,"","","");legacy.history_.useManualPersistenceForTests();legacy.loadState();
            assert(!legacy.devices_[device.ieeeAddress].light.read().on&&!legacy.deviceCacheDirty_);
            legacy.stop();assert(access("/tmp/microhad-values.json",F_OK)==0);
            unlink("/tmp/microhad-values.json");
        }
        unlink(cache.c_str());unlink((std::string(directory)+"/devices.json").c_str());assert(rmdir(directory)==0);
    }

    static void genericThermostats() {
        puts("unit: generic Zigbee/Matter thermostat discovery, capabilities, cooling, schedules and transport preparation");
        ThermostatValues absent, zero;
        assert(absent.get(ThermostatField::local_temperature,99)==99);
        zero[ThermostatField::local_temperature]=0;
        assert(zero.count(ThermostatField::local_temperature));
        assert(zero.get(ThermostatField::local_temperature,99)==0 && !(zero==absent));
        zero.erase(ThermostatField::local_temperature);
        assert(zero==absent);
        zero[ThermostatField::local_temperature]=123;
        zero.erase(ThermostatField::local_temperature);
        assert(zero==absent); // erased storage must not affect snapshot reuse
        unsigned long fieldCount=0;
        const ThermostatAttribute* fields=thermostatAttributes(&fieldCount);
        assert(fieldCount==ThermostatField::Count);
        for(unsigned i=0;i<fieldCount;++i) {
            assert(fields[i].id==static_cast<ThermostatField::Id>(i));
            assert(thermostatAttribute(fields[i].name)==&fields[i]);
        }
        ThermostatPending pending;
        for(unsigned i=0;i<ThermostatPendingCount;++i) {
            assert(thermostatPendingId(thermostatPendingName(i))==i);
            pending.insert(i);assert(pending.count(i));pending.erase(i);assert(!pending.count(i));
        }
        assert(thermostatPendingId("unknown")==ThermostatPendingCount);
        pending.insert(ThermostatPendingCount);assert(pending.empty());

        Device d; d.ieeeAddress="0x00124b0000000071"; d.zigbee.ensure().networkAddress=0x7771;
        EndpointDescriptor endpoint; endpoint.endpoint=3; endpoint.profileId=0x104;
        endpoint.inputClusters.push_back(0x201);endpoint.inputClusters.push_back(6);d.zigbee.ensure().endpoints.push_back(endpoint);
        char directory[]="/tmp/microhad-generic-thermostat-XXXXXX";assert(mkdtemp(directory));
        Gateway g("/dev/null",115200,directory,"","","");g.classifyDevice(&d);
        assert(isThermostat(d) && !isTrvzb(d) && !d.light.present() && d.kind=="thermostat");
        unsigned observed=0;std::vector<unsigned char> local;local.push_back(0x34);local.push_back(8);
        assert(applyZclAttribute(&d,0x201,0,local,100,&observed));assert(d.temperature.read().celsius==21 && d.temperature.read().observation.at==100);
        d.thermostat.ensure().values[ThermostatField::control_sequence_of_operation]=4;
        d.thermostat.ensure().values[ThermostatField::occupied_heating_setpoint]=20;d.thermostat.ensure().values[ThermostatField::occupied_cooling_setpoint]=25;
        d.thermostat.ensure().values[ThermostatField::system_mode]=1;d.thermostat.ensure().values[ThermostatField::min_heat_setpoint_limit]=5;d.thermostat.ensure().values[ThermostatField::max_heat_setpoint_limit]=40;
        std::string error;std::vector<unsigned char> encoded;
        assert(thermostatEncode(d,"occupied_heating_setpoint","22.25",&encoded,&error));
        assert(!thermostatEncode(d,"occupied_heating_setpoint","24",&encoded,&error));
        assert(!thermostatEncode(d,"child_lock","1",&encoded,&error));
        assert(thermostatEncode(d,"system_mode","3",&encoded,&error));
        d.thermostat.ensure().values[ThermostatField::control_sequence_of_operation]=2;
        assert(!thermostatSupported(d,*thermostatAttribute(ThermostatField::occupied_cooling_setpoint)));
        assert(!thermostatModeSupported(d,3));d.thermostat.ensure().values[ThermostatField::control_sequence_of_operation]=4;
        d.thermostat.ensure().scheduleSupported=true;
        assert(thermostatScheduleForDevice(d,"{\"mode\":3,\"transitions\":[{\"time\":60,\"heat\":20,\"cool\":25}]}",1,&encoded));
        assert(applyThermostatSchedule(&d,encoded));assert(d.thermostat.read().scheduleDetails.count(1));
        assert(!thermostatScheduleForDevice(d,"{\"mode\":3,\"transitions\":[{\"time\":60,\"heat\":24,\"cool\":25}]}",1,&encoded));
        Device matter=d;matter.source="matter";matter.ieeeAddress="matter:test:endpoint";matter.matter.ensure().node="0000000000000072";matter.matter.ensure().endpoint=2;matter.matter.ensure().servers.push_back(513);
        assert(isThermostat(matter));
        assert(applyMatterThermostat(&matter,65531,"thermostat_list","[0,17,18,27,28,65532]",100));
        assert(applyMatterThermostat(&matter,65532,"thermostat","35",100));
        assert(thermostatModeSupported(matter,1) && thermostatModeSupported(matter,3));
        assert(applyMatterThermostat(&matter,0,"thermostat","null",101));assert(!matter.temperature.read().known);
        assert(applyMatterThermostat(&matter,0,"thermostat","-525",102));assert(matter.temperature.read().celsius==-5.25);
        g.devices_[matter.ieeeAddress]=matter;MatterProto::MessageBody request;
        assert(g.setThermostat(matter.ieeeAddress,"occupied_cooling_setpoint","26.5",&error,&request));
        assert(request.device_command_request.present&&request.device_command_request.value.value.value=="26.5");
        assert(g.thermostatResult(matter.matter.read().node,2,"occupied_cooling_setpoint","timeout"));
        assert(!g.devices_[matter.ieeeAddress].thermostat.read().error.empty());
        assert(!g.setThermostat(matter.ieeeAddress,"child_lock","1",&error,&request));
        g.devices_[d.ieeeAddress]=d;
        assert(g.captureView().statusJson().find("\"has_thermostat\":true")!=std::string::npos);
        rmdir(directory);
    }
    static void trvzb() {
        puts("unit: TRVZB confirmation, sensor freshness, fallback, restart and persistence");
        char directory[]="/tmp/microhad-trvzb-XXXXXX"; assert(mkdtemp(directory));
        std::string base(directory), error;
        Gateway g("/dev/null",115200,base,"","","");
        g.online_=true; g.networkFormed_=true; g.coordinatorInitializationStep_=0;
        Device valve; valve.ieeeAddress="0x00124b0000000042"; valve.modelId="TRVZB";
        valve.softwareBuildId="1.3.0"; valve.zigbee.ensure().networkAddress=0x1234;
        EndpointDescriptor e; e.endpoint=1; e.profileId=0x0104; e.inputClusters.push_back(0x0201);
        e.inputClusters.push_back(0xfc11); e.inputClusters.push_back(6); valve.zigbee.ensure().endpoints.push_back(e);
        g.classifyDevice(&valve); assert(!valve.light.present() && valve.temperature.present() && valve.kind=="thermostat");
        g.devices_[valve.ieeeAddress]=valve;
        std::vector<unsigned char> reportingOk(1,0);
        assert(!g.handleThermostatMessage(valve.ieeeAddress,1,0x0201,8,0,1,7,reportingOk));
        Device sensor; sensor.ieeeAddress="shelly:room:temperature:0"; sensor.source="shelly";
        sensor.temperature.setPresent(true); sensor.temperature.ensure().known=true; sensor.temperature.ensure().celsius=21.26;
        sensor.temperature.ensure().observation.at=time(0); g.devices_[sensor.ieeeAddress]=sensor;
        assert(g.setThermostat(valve.ieeeAddress,"temperature_source",sensor.ieeeAddress,&error));
        assert(!g.setThermostat(valve.ieeeAddress,"external_temperature_input","20",&error));
        assert(!g.setThermostat(valve.ieeeAddress,"stale_minutes","4",&error));
        assert(g.setThermostat(valve.ieeeAddress,"stale_minutes","10",&error));
        g.pollThermostats(); assert(g.thermostatOperations_.size()==1);
        ThermostatOperationForTest(g, "external_temperature_input", 2130);
        assert(g.thermostatOperations_.size()==1 && g.thermostatOperations_.front().pendingId==ThermostatField::temperature_sensor_select);
        g.pollThermostats(); ThermostatOperationForTest(g,"temperature_sensor_select",1);
        Device& t=g.devices_[valve.ieeeAddress];
        assert(t.thermostat.read().values[ThermostatField::temperature_sensor_select]==1 && t.thermostat.read().lastForwardValue==21.3);
        g.pollThermostats(); assert(g.thermostatOperations_.empty());
        // A changed reading is coalesced for one minute.
        g.devices_[sensor.ieeeAddress].temperature.ensure().celsius=22;
        g.pollThermostats(); assert(g.thermostatOperations_.empty());
        t.thermostat.ensure().lastForwardAt=time(0)-61;
        g.pollThermostats(); assert(g.thermostatOperations_.size()==1);
        ThermostatOperationForTest(g,"external_temperature_input",2200);
        g.pollThermostats(); ThermostatOperationForTest(g,"temperature_sensor_select",1);
        // General traffic cannot keep an old temperature fresh.
        g.devices_[sensor.ieeeAddress].temperature.ensure().observation.at=time(0)-601;
        g.devices_[sensor.ieeeAddress].lastSeen=time(0); t.thermostat.ensure().lastForwardAt=0;
        g.pollThermostats();
        assert(g.thermostatOperations_.front().pendingId==ThermostatField::temperature_sensor_select);
        assert(t.thermostat.read().values[ThermostatField::temperature_sensor_select]==1); // fallback not yet confirmed
        ThermostatOperationForTest(g,"temperature_sensor_select",0);
        g.pollThermostats(); assert(t.thermostat.read().status.find("built-in sensor confirmed")!=std::string::npos);
        // Fresh identical readings recover; selecting the valve itself is rejected.
        g.devices_[sensor.ieeeAddress].temperature.ensure().observation.at=time(0); t.thermostat.ensure().lastForwardAt=0;
        g.pollThermostats(); assert(g.thermostatOperations_.front().pendingId==ThermostatField::external_temperature_input);
        assert(!g.setThermostat(valve.ieeeAddress,"temperature_source",valve.ieeeAddress,&error));
        g.thermostatOperations_.clear();
        // Actual observations are transient; a reconstructed source must first report.
        Device zigbeeSensor; zigbeeSensor.ieeeAddress="0x00124b0000000043"; zigbeeSensor.temperature.setPresent(true);
        g.devices_[zigbeeSensor.ieeeAddress]=zigbeeSensor;
        std::vector<unsigned char> measured; measured.push_back(0x34); measured.push_back(8);
        assert(g.applyAttributeReport(zigbeeSensor.ieeeAddress,0x0402,0,measured));
        assert(g.devices_[zigbeeSensor.ieeeAddress].temperature.read().observation.at>0);
        assert(g.setThermostat(valve.ieeeAddress,"temperature_source",zigbeeSensor.ieeeAddress,&error));
        assert(g.setThermostat(valve.ieeeAddress,"temperature_source",sensor.ieeeAddress,&error));
        Device matterSensor; matterSensor.ieeeAddress="matter:room:1"; matterSensor.source="matter";
        matterSensor.matter.ensure().node="0000000000000044"; matterSensor.matter.ensure().endpoint=1; matterSensor.temperature.setPresent(true);
        g.devices_[matterSensor.ieeeAddress]=matterSensor;
        MatterReading reading; reading.node="0000000000000044"; reading.endpoint=1; reading.cluster=1026; reading.known=true; reading.numberValue=20;
        assert(g.applyMatterReading(reading,&error));
        assert(g.devices_[matterSensor.ieeeAddress].temperature.read().observation.at>0);
        assert(g.setThermostat(valve.ieeeAddress,"temperature_source",matterSensor.ieeeAddress,&error));
        reading.known=false; assert(g.applyMatterReading(reading,&error));
        assert(g.devices_[matterSensor.ieeeAddress].temperature.read().observation.at==0);
        assert(g.setThermostat(valve.ieeeAddress,"temperature_source",sensor.ieeeAddress,&error));
        g.devices_[sensor.ieeeAddress].temperature.ensure().observation.at=0;
        t.thermostat.ensure().lastForwardAt=0;
        g.pollThermostats(); assert(g.thermostatOperations_.empty());
        // Initial background reads cannot delay a required fallback.
        t.thermostat.ensure().values.erase(ThermostatField::temperature_sensor_select);
        g.queueThermostatReadings(t);
        g.pollThermostats();
        assert(g.thermostatOperations_.front().pendingId==ThermostatField::temperature_sensor_select && g.thermostatOperations_.front().command==2);
        ThermostatOperationForTest(g,"temperature_sensor_select",0);
        g.thermostatOperations_.clear(); t.thermostat.ensure().pending.clear();
        assert(g.setThermostat(valve.ieeeAddress,"occupied_heating_setpoint","21.5",&error));
        assert(!g.setThermostat(valve.ieeeAddress,"occupied_heating_setpoint","22",&error));
        // Reject responses from another endpoint, wrong sequence or manufacturer.
        g.pollThermostats();
        unsigned char seq=g.thermostatOperations_.front().sequence;
        std::vector<unsigned char> ok(1,0);
        g.handleThermostatMessage(valve.ieeeAddress,2,0x0201,8,0,seq,4,ok);
        assert(!g.thermostatOperations_.front().readback);
        g.handleThermostatMessage(valve.ieeeAddress,1,0x0201,8,0x1286,seq,4,ok);
        assert(!g.thermostatOperations_.front().readback);
        g.handleThermostatMessage(valve.ieeeAddress,1,0x0201,8,0,seq+1,4,ok);
        assert(!g.thermostatOperations_.front().readback);
        ThermostatOperationForTest(g,"occupied_heating_setpoint",2150);
        assert(t.thermostat.read().values[ThermostatField::occupied_heating_setpoint]==21.5);
        g.queueThermostatOperation(t,ThermostatField::child_lock,0xfc11,0,std::vector<unsigned char>(2,0));
        g.thermostatOperations_.front().attempts=3;
        g.thermostatOperations_.front().deadline=time(0)-1;
        g.pollThermostats(); assert(g.thermostatOperations_.empty() && !t.thermostat.read().error.empty());
        std::vector<Device> stored; stored.push_back(t); stored.push_back(g.devices_[sensor.ieeeAddress]);
        assert(saveDevices(base+"/trvzb.json",stored,&error)); stored.clear();
        assert(loadDevices(base+"/trvzb.json",&stored,&error));
        assert(stored[0].thermostatSourceId==sensor.ieeeAddress && stored[0].thermostatStaleMinutes==10);
        assert(stored[1].temperature.read().observation.at==0);
        g.devices_.erase(sensor.ieeeAddress); t.thermostat.ensure().values[ThermostatField::temperature_sensor_select]=1; t.thermostat.ensure().lastForwardAt=0;
        g.pollThermostats(); assert(g.thermostatOperations_.front().pendingId==ThermostatField::temperature_sensor_select);
        g.thermostatOperations_.clear();
        t.thermostat.ensure().pending.clear();
        assert(g.setThermostat(valve.ieeeAddress,"temperature_source","builtin",&error));
        assert(t.thermostatSourceId.empty());
        g.pollThermostats();
        assert(!g.setThermostat(valve.ieeeAddress,"temperature_source","manual",&error));
        ThermostatOperationForTest(g,"temperature_sensor_select",0);
        assert(g.setThermostat(valve.ieeeAddress,"schedule_1","00:00/18 07:00/21",&error));
        g.pollThermostats();
        Gateway::ThermostatOperation schedule=g.thermostatOperations_.front();
        std::vector<unsigned char> ack; ack.push_back(1); ack.push_back(0);
        g.handleThermostatMessage(valve.ieeeAddress,1,0x0201,8,0,schedule.sequence,0x0b,ack);
        assert(g.thermostatOperations_.front().readback);
        g.pollThermostats(); schedule=g.thermostatOperations_.front();
        g.handleThermostatMessage(valve.ieeeAddress,1,0x0201,9,0,schedule.sequence,0,schedule.payload);
        assert(g.thermostatOperations_.empty() && t.thermostat.read().schedules.find(1)->second=="00:00/18 07:00/21");
        assert(t.thermostat.read().schedules.size()==1);
        assert(g.setThermostat(valve.ieeeAddress,"child_lock","1",&error)); g.pollThermostats();
        Gateway::ThermostatOperation rejected=g.thermostatOperations_.front();
        std::vector<unsigned char> rejection; rejection.push_back(0x86); rejection.push_back(0); rejection.push_back(0);
        g.handleThermostatMessage(valve.ieeeAddress,1,0xfc11,8,0,rejected.sequence,4,rejection);
        assert(g.thermostatOperations_.empty() && t.thermostat.read().unsupported.count(ThermostatField::child_lock));
        assert(!t.thermostat.read().error.empty() && t.thermostat.read().pending.empty());
        std::vector<unsigned char> wrongType; wrongType.push_back(0); wrongType.push_back(0);
        wrongType.push_back(0x21); wrongType.push_back(0x34); wrongType.push_back(8);
        g.handleThermostatMessage(valve.ieeeAddress,1,0x0201,0x18,0,100,0x0a,wrongType);
        assert(!t.temperature.read().known);
        unlink((base+"/trvzb.json").c_str()); unlink((base+"/devices.json").c_str()); rmdir(base.c_str());
    }
    static void ThermostatOperationForTest(Gateway& g, const std::string& field, unsigned long raw) {
        assert(!g.thermostatOperations_.empty());
        Gateway::ThermostatOperation op=g.thermostatOperations_.front(); assert(op.pendingId==thermostatPendingId(field));
        std::vector<unsigned char> ok(1,0);
        g.handleThermostatMessage(op.address,op.endpoint,op.cluster,8,0,op.sequence,4,ok);
        assert(g.thermostatOperations_.front().readback);
        g.pollThermostats(); op=g.thermostatOperations_.front();
        const ThermostatAttribute* a=thermostatAttribute(field); assert(a);
        std::vector<unsigned char> response;
        response.push_back(a->attribute&255); response.push_back(a->attribute>>8);
        response.push_back(0); response.push_back(a->type);
        unsigned int size=a->type==0x29 || a->type==0x21 || a->type==0x19 ? 2 : a->type==0x23 ? 4 : 1;
        for (unsigned int i=0;i<size;++i) response.push_back(static_cast<unsigned char>(raw>>(8*i)));
        g.handleThermostatMessage(op.address,op.endpoint,op.cluster,8,0,op.sequence,1,response);
    }

    static void deviceDeletion() {
        puts("unit: device setup state, targeted deletion, local fallback and durable reference cleanup");
        char directory[] = "/tmp/microhad-device-deletion-XXXXXX"; assert(mkdtemp(directory));
        const std::string base(directory), id = "0x00124b0000000020", other = "0x00124b0000000021";
        const std::string values = "/tmp/microhad-values.json", backup = base + "/values-backup.json";
        const bool hadValues = rename(values.c_str(), backup.c_str()) == 0;
        std::string error, json;
        Gateway gateway("/dev/null", 115200, base, "", "", "");
        Device device; device.ieeeAddress = id; device.name = "Unfinished"; device.zigbee.ensure().networkAddress = 0x1234;
        gateway.devices_[id] = device; gateway.nodeAddresses_[0x1234] = id;
        device.ieeeAddress = other; device.zigbee.ensure().initialInterviewComplete = true; device.zigbee.ensure().networkAddress = 0xffff;
        gateway.devices_[other] = device;
        assert(gateway.statusJson().find("\"setup_state\":\"incomplete\"") != std::string::npos);
        assert(!gateway.deleteDevice(id, false, &error)); // Offline requires explicit local fallback.
        gateway.online_ = true; gateway.networkFormed_ = true;
        SceneGroup group; group.id = 1; group.name = "Room"; group.members.push_back(id); group.members.push_back(other);
        Scene scene; scene.id = 1; scene.name = "Evening"; SceneLightState state; state.ieeeAddress = id; scene.states.push_back(state); group.scenes.push_back(scene);
        gateway.sceneGroups_.push_back(group);
        gateway.sceneRetries_[1]; gateway.sceneRetries_[2];
        RemoteSceneBinding binding; binding.ieeeAddress = id; binding.groupId = 1; gateway.sceneBindings_.push_back(binding);
        NativeRemoteBinding native; native.ieeeAddress = id; native.groupId = 1; gateway.nativeBindings_.push_back(native);
        gateway.nextContactAlertAt_[id] = time(0); gateway.sceneProvisioning_[id];
        assert(saveDashboardConfiguration(base + "/dashboard.json", "{\"widgets\":[{\"id\":\"a\",\"kind\":\"device\",\"target\":\"" + id + "\"},{\"id\":\"b\",\"kind\":\"chart\",\"target\":\"" + other + "\"}],\"gateway_widget_initialized\":true}"));
        { std::ofstream history((base + "/history.values").c_str()); history << "keep history"; }
        assert(gateway.deleteDevice(id, false, &error));
        assert(!gateway.deleteDevice(id, true, &error)); // Do not override a pending unpair.
        assert(gateway.sceneProvisioning_.count(id) == 0);
        const unsigned char sequence = gateway.deviceDeletions_[id].sequence;
        std::vector<unsigned char> reply(2, 0); reply[0] = sequence + 1;
        gateway.handleApsMessage(0x1234, 0, 0x8034, 0, 0, 0, 0, 0, 0, 0, 0, reply);
        assert(gateway.devices_.count(id));
        reply[0] = sequence;
        gateway.handleApsMessage(0x1234, 0, 0x8034, 0, 0, 0, 0, 0, 0, 0, 0, reply);
        assert(gateway.deviceDeletionStatus(id, &json, &error) && json.find("succeeded") != std::string::npos);
        assert(!gateway.devices_.count(id) && gateway.devices_.count(other));
        assert(!gateway.nodeAddresses_.count(0x1234) && !gateway.nextContactAlertAt_.count(id));
        assert(gateway.sceneGroups_[0].members.size() == 1 && gateway.sceneGroups_[0].scenes[0].states.empty());
        assert(gateway.sceneBindings_.empty() && gateway.nativeBindings_.empty());
        assert(!gateway.sceneRetries_.count(1) && gateway.sceneRetries_.count(2));
        assert(readTextFile(base + "/dashboard.json", &json) && json.find(id) == std::string::npos && json.find(other) != std::string::npos);
        assert(json.find("gateway_widget_initialized") != std::string::npos);
        assert(saveDashboardConfiguration(base + "/dashboard.json", "{\"label\":\"widgets\",\"metadata\":[{\"target\":\"" + id + "\"}],\"widg\\u0065ts\":[{\"target\":\"" + id + "\"},{\"target\":\"" + other + "\"}]}"));
        assert(pruneDashboardDevices(base + "/dashboard.json", std::vector<std::string>(1, id), &error));
        assert(readTextFile(base + "/dashboard.json", &json));
        assert(json.find("\"metadata\":[{\"target\":\"" + id + "\"}]") != std::string::npos);
        assert(json.find("\"widg\\u0065ts\":[{\"target\":\"" + other + "\"}]") != std::string::npos);

        assert(readTextFile(base + "/history.values", &json) && json == "keep history");
        Gateway restarted("/dev/null", 115200, base, "", "", ""); restarted.loadState();
        assert(!restarted.devices_.count(id) && restarted.devices_.count(other));
        assert(restarted.devices_[other].zigbee.read().initialInterviewComplete);
        // Timed-out attempts retain the record; explicit local fallback removes it.
        gateway.devices_[id] = device; gateway.devices_[id].ieeeAddress = id; gateway.devices_[id].zigbee.ensure().networkAddress = 0x1234;
        gateway.nodeAddresses_[0x1234] = id;
        assert(gateway.deleteDevice(id, false, &error)); gateway.deviceDeletions_[id].deadline = time(0) - 1; gateway.poll();
        assert(gateway.deviceDeletionStatus(id, &json, &error) && json.find("failed") != std::string::npos && gateway.devices_.count(id));
        assert(gateway.deleteDevice(id, true, &error) && !gateway.devices_.count(id));
        // A storage failure must not claim success or discard the visible record.
        Gateway broken("/dev/null", 115200, base + "/missing", "", "", ""); broken.devices_[id] = device;
        assert(broken.deleteDevice(id, true, &error));
        assert(broken.deviceDeletionStatus(id, &json, &error) && json.find("failed") != std::string::npos && broken.devices_.count(id));
        // Completed discovery of an unsupported device is still completed setup.
        Gateway unsupported("/dev/null", 115200, base, "", "", ""); unsupported.devices_[id] = device;
        unsupported.devices_[id].zigbee.ensure().initialInterviewComplete = false;
        EndpointDescriptor endpoint; endpoint.endpoint = 1; unsupported.devices_[id].zigbee.ensure().endpoints.push_back(endpoint);
        unsupported.interviews_[0x1234].ieeeAddress = id; unsupported.buildConfigurationOperations(0x1234);
        assert(unsupported.devices_[id].zigbee.read().initialInterviewComplete && unsupported.interviews_.empty());
        std::vector<Device> loaded; assert(loadDevices(base + "/devices.json", &loaded, &error) && loaded[0].zigbee.read().initialInterviewComplete);
        assert(readTextFile(base + "/devices.json", &json));
        const std::string marker = ",\"initial_interview_complete\":true"; json.erase(json.find(marker), marker.size());
        { std::ofstream legacy((base + "/devices.json").c_str()); legacy << json; }
        assert(loadDevices(base + "/devices.json", &loaded, &error) && loaded[0].zigbee.read().initialInterviewComplete);
        // Matter removal retires every endpoint and node alias, but preserves other protocols.
        const std::string node = "0000000000000002", prefix = "matter:0000000000000001:" + node;
        for (int i = 0; i < 2; ++i) { Device m; m.source = "matter"; m.matter.ensure().node = node; m.ieeeAddress = prefix + (i == 0 ? ":0001" : ":0002"); unsupported.devices_[m.ieeeAddress] = m; }
        assert(unsupported.reconcileMatterNodes(std::vector<std::string>(), &error));
        assert(unsupported.devices_.size() == 1 && unsupported.devices_.count(id));
        const char* files[] = {"devices.json", "scenes.conf", "dashboard.json", "history.values"};
        for (unsigned int i = 0; i < 4; ++i) unlink((base + "/" + files[i]).c_str());
        unlink(values.c_str()); if (hadValues) assert(rename(backup.c_str(), values.c_str()) == 0); rmdir(directory);
    }

    static void contactAlertsSurviveRestart() {
        char directory[] = "/tmp/microhad-contact-restart-XXXXXX";
        assert(mkdtemp(directory));
        const std::string values = "/tmp/microhad-values.json";
        const std::string backup = std::string(directory) + "/values-backup.json";
        const bool hadValues = rename(values.c_str(), backup.c_str()) == 0;
        std::string error;
        std::vector<Device> saved(4);
        const char* ids[] = {"open", "closed", "trigger", "disabled"};
        for (unsigned int i = 0; i < saved.size(); ++i) {
            saved[i].ieeeAddress = ids[i];
            saved[i].contact.setPresent(true);
            saved[i].contact.ensure().known = true;
            saved[i].contact.ensure().open = i != 1;
            saved[i].contactInitialAlertMinutes = 20;
            saved[i].contactReminderAlertMinutes = 7;
        }
        saved[2].contactTriggerMode = true;
        saved[3].contactReminderAlertMinutes = 0;
        assert(saveDevices(std::string(directory) + "/devices.json", saved, &error));
        assert(saveDevices(values, saved, &error));
        NotificationWorker worker(directory, "", "", "");
        assert(worker.start(&error));
        for (unsigned int disabled = 0; disabled < 2; ++disabled) {
            assert(worker.setEnabled(disabled == 0, &error));
            assert(saveDeviceCache("/tmp/microhad-values.pb", saved, &error));
            Gateway restarted("/dev/null", 115200, directory, "", "", "", &worker);
            const long before = time(0);
            restarted.loadState();
            assert(restarted.nextContactAlertAt_.size() == 1);
            assert(restarted.contactOpenedAt_.size() == 1);
            const long deadline = restarted.nextContactAlertAt_["open"];
            assert(deadline >= before + 20 * 60 && deadline <= time(0) + 20 * 60);
            assert(restarted.nextContactTriggerAt_.empty());
            assert(logOccurrences(restarted, "Notification queued:") == 0);
            std::vector<unsigned char> open(2, 0);
            open[0] = 1;
            assert(restarted.applyIasZoneStatus("open", open));
            assert(restarted.nextContactAlertAt_["open"] == deadline);
            restarted.nextContactAlertAt_["open"] = time(0) - 1;
            restarted.poll();
            assert(restarted.nextContactAlertAt_["open"] >= time(0) + 7 * 60 - 1);
            assert(logOccurrences(restarted, "Notification queued: Window open") == (disabled ? 0 : 1));
            assert(logOccurrences(restarted, "Notification suppressed:") == 0);
            restarted.nextContactAlertAt_["open"] = time(0) - 1;
            restarted.poll();
            assert(logOccurrences(restarted, "Notification queued: Window open") == (disabled ? 0 : 2));
            std::vector<unsigned char> closed(2, 0);
            assert(restarted.applyIasZoneStatus("open", closed));
            assert(restarted.nextContactAlertAt_.empty());
            assert(restarted.contactOpenedAt_.empty());
            // Restore the original snapshot for the disabled-push iteration.
            assert(saveDevices(values, saved, &error));
        }
        worker.stop();
        unlink(values.c_str());
        if (hadValues) assert(rename(backup.c_str(), values.c_str()) == 0);
        unlink((std::string(directory) + "/devices.json").c_str());
        unlink((std::string(directory) + "/notifications.json").c_str());
        rmdir(directory);
    }
    static void reportingConfigurationPersistence() {
        char directory[] = "/tmp/microhad-reporting-config-XXXXXX";
        assert(mkdtemp(directory));
        const std::string values = "/tmp/microhad-values.json";
        const std::string backup = std::string(directory) + "/values-backup.json";
        bool hadValues = rename(values.c_str(), backup.c_str()) == 0;
        Gateway gateway("/dev/null", 115200, directory, "", "", "");
        const std::string ieee = "0xb43a31fffe930f98";
        const unsigned short node = 0xb70f;
        const unsigned char eui[] = {0x98, 0x0f, 0x93, 0xfe, 0xff, 0x31, 0x3a, 0xb4};
        Device& device = gateway.devices_[ieee];
        device.ieeeAddress = ieee; device.zigbee.ensure().networkAddress = node;
        EndpointDescriptor endpoint;
        endpoint.endpoint = 1; endpoint.profileId = 0x0104;
        endpoint.inputClusters.push_back(6); endpoint.inputClusters.push_back(8);
        device.zigbee.ensure().endpoints.push_back(endpoint);
        gateway.nodeAddresses_[node] = ieee;
        gateway.coordinatorIeeeAddress_.assign(8, 1);
        gateway.startInterview(node, false);
        assert(gateway.interviews_.count(node));
        assert(device.zigbee.read().configuredSignature.empty());
        while (gateway.interviews_.count(node)) gateway.completeInterviewOperation(node);
        assert(device.zigbee.read().configuredSignature == gateway.deviceConfigurationSignature(device));
        std::vector<Device> saved;
        std::string error;
        assert(loadDevices(std::string(directory) + "/devices.json", &saved, &error));
        assert(saved.size() == 1 && saved[0].zigbee.read().configuredSignature == device.zigbee.read().configuredSignature);
        // Ordinary announcements and short-address migrations retain completed bindings.
        gateway.registerNode(node, eui);
        assert(gateway.interviews_.empty());
        gateway.registerNode(0xb710, eui);
        assert(gateway.interviews_.empty());
        // Restarts load the successful marker; RAM cannot override its persistent value.
        Gateway restarted("/dev/null", 115200, directory, "", "", "");
        restarted.loadState(); restarted.coordinatorIeeeAddress_.assign(8, 1);
        assert(restarted.devices_[ieee].zigbee.read().configuredSignature == device.zigbee.read().configuredSignature);
        restarted.startInterview(0xb710, false);
        assert(restarted.interviews_.empty());
        // Changed endpoint configuration must be applied again.
        device.zigbee.ensure().endpoints[0].inputClusters.push_back(0x0402);
        gateway.startInterview(0xb710, false);
        assert(gateway.interviews_.count(0xb710));
        gateway.interviews_.erase(0xb710);
        device.zigbee.ensure().endpoints[0].inputClusters.pop_back();
        // An explicit request forces discovery/configuration even with a matching marker.
        gateway.online_ = true; gateway.networkFormed_ = true; gateway.coordinatorInitializationStep_ = 0;
        assert(gateway.reconfigureDevice(ieee, &error));
        assert(gateway.interviews_[0xb710].forceConfiguration);
        assert(gateway.interviews_[0xb710].operations[0].type == Gateway::InterviewActiveEndpoints);
        gateway.interviews_.erase(0xb710);
        device.zigbee.ensure().configuredSignature.clear();
        // Rejected binding responses must not create a successful marker.
        gateway.startInterview(0xb710, false);
        Gateway::InterviewOperation& bind = gateway.interviews_[0xb710].operations[0];
        bind.transactionSequenceAssigned = true; bind.transactionSequence = 7;
        std::vector<unsigned char> rejectedBind(2, 0);
        rejectedBind[0] = 7; rejectedBind[1] = 0x84;
        gateway.handleApsMessage(0xb710, 0, 0x8021, 0, 0, 0, 0, 0, 0, 200, -50, rejectedBind);
        assert(gateway.interviews_[0xb710].configurationFailed);
        while (gateway.interviews_.count(0xb710)) gateway.completeInterviewOperation(0xb710);
        assert(device.zigbee.read().configuredSignature.empty());
        // Nor may rejected or empty Configure Reporting responses create a marker.
        for (unsigned int empty = 0; empty < 2; ++empty) {
            gateway.startInterview(0xb710, false);
            while (gateway.interviews_[0xb710].operations[gateway.interviews_[0xb710].operationIndex].type !=
                   Gateway::InterviewConfigureReporting) gateway.completeInterviewOperation(0xb710);
            Gateway::InterviewOperation& reporting = gateway.interviews_[0xb710].operations[gateway.interviews_[0xb710].operationIndex];
            reporting.transactionSequenceAssigned = true; reporting.transactionSequence = 8;
            const unsigned char bytes[] = {0x18, 8, 7, 0x86, 0, 0, 0};
            std::vector<unsigned char> response(bytes, bytes + (empty ? 3 : sizeof(bytes)));
            gateway.handleApsMessage(0xb710, 0x0104, reporting.cluster, 1, 1, 0, 0, 0, 0, 200, -50, response);
            assert(gateway.interviews_[0xb710].configurationFailed);
            while (gateway.interviews_.count(0xb710)) gateway.completeInterviewOperation(0xb710);
            assert(device.zigbee.read().configuredSignature.empty());
        }
        unlink((std::string(directory) + "/devices.json").c_str());
        unlink(values.c_str());
        if (hadValues) assert(rename(backup.c_str(), values.c_str()) == 0);
        assert(rmdir(directory) == 0);
    }
    static void freshJoinRestoresConfiguration() {
        char directory[] = "/tmp/microhad-fresh-join-XXXXXX";
        assert(mkdtemp(directory));
        Gateway gateway("/dev/null", 115200, directory, "", "", "");
        const std::string ieee = "0x5c0272fffeca3ba7";
        const unsigned short node = 0xb589;
        const unsigned char eui[] = {0xa7,0x3b,0xca,0xfe,0xff,0x72,0x02,0x5c};
        Device& device = gateway.devices_[ieee];
        device.ieeeAddress = ieee; device.zigbee.ensure().networkAddress = node;
        EndpointDescriptor endpoint; endpoint.endpoint = 1; endpoint.profileId = 0x0104;
        endpoint.inputClusters.push_back(4); endpoint.inputClusters.push_back(5);
        endpoint.inputClusters.push_back(6); endpoint.inputClusters.push_back(8);
        device.zigbee.ensure().endpoints.push_back(endpoint);
        device.zigbee.ensure().configuredSignature = gateway.deviceConfigurationSignature(device);
        gateway.nodeAddresses_[node] = ieee;
        SceneGroup group; group.id = 2; group.name = "Kitchen"; group.members.push_back(ieee);
        Scene scene; scene.id = 1; scene.name = "Work";
        SceneLightState light; light.ieeeAddress = ieee; light.on = true; light.brightnessPercent = 70;
        scene.states.push_back(light); group.scenes.push_back(scene); gateway.sceneGroups_.push_back(group);
        Gateway::SceneProvisionState unrelated; unrelated.waitingForTraffic = true;
        gateway.sceneProvisioning_["unrelated"] = unrelated;
        // An ordinary announcement and a preserving join keep the cached settings.
        gateway.registerNode(node, eui);
        assert(gateway.interviews_.empty());
#if MICROHAD_ZIGGURAT_ENABLED
        gateway.useZiggurat("/tmp/fresh-join-test.sock");
        ZigguratClient::Event joined(ZigguratClient::Event::Joined);
        joined.payload.push_back(0x89); joined.payload.push_back(0xb5);
        joined.payload.insert(joined.payload.end(), eui, eui + 8);
        joined.payload.push_back(0); joined.payload.push_back(0); joined.payload.push_back(3);
        gateway.ziggurat_->events_.push_back(joined); gateway.pollZiggurat();
        assert(gateway.interviews_.empty());
        joined.payload[12] |= 8;
        gateway.ziggurat_->events_.push_back(joined); gateway.pollZiggurat();
#else
        gateway.registerNode(node, eui, true);
#endif
        assert(device.zigbee.read().configuredSignature.empty());
        assert(gateway.interviews_[node].forceConfiguration);
        assert(gateway.interviews_[node].operations[0].type == Gateway::InterviewActiveEndpoints);
        // Complete discovery with the saved descriptors and exercise real planning.
        Gateway::InterviewState& interview = gateway.interviews_[node];
        interview.operations.clear(); interview.operationIndex = 0;
        gateway.buildConfigurationOperations(node);
        assert(!gateway.interviews_[node].operations.empty());
        while (gateway.interviews_.count(node)) gateway.completeInterviewOperation(node);
        assert(device.zigbee.read().configuredSignature == gateway.deviceConfigurationSignature(device));
        assert(gateway.sceneProvisioning_.count("unrelated"));
        const Gateway::SceneProvisionState& restored = gateway.sceneProvisioning_[ieee];
        assert(restored.commands.size() == 2);
        assert(restored.commands[0].cluster == 4 && restored.commands[0].groupId == 2);
        assert(restored.commands[1].cluster == 5 && restored.commands[1].sceneId == 1);
        // The subsequent duplicate announcement must not restart restoration.
        gateway.registerNode(node, eui);
        assert(gateway.interviews_.empty());
        assert(gateway.sceneProvisioning_[ieee].commands.size() == 2);
        // A freshly associated remote must recover its native output bindings,
        // without clearing another remote's queued work.
        const std::string remoteIeee = "0x70ac08fffe75827b";
        const unsigned char remoteEui[] = {0x7b,0x82,0x75,0xfe,0xff,0x08,0xac,0x70};
        Device& remote = gateway.devices_[remoteIeee];
        remote.ieeeAddress = remoteIeee; remote.remote.setPresent(true);
        remote.zigbee.ensure().networkAddress = 0xd938;
        EndpointDescriptor remoteEndpoint; remoteEndpoint.endpoint = 1; remoteEndpoint.profileId = 0x0104;
        remoteEndpoint.outputClusters.push_back(6); remoteEndpoint.outputClusters.push_back(8);
        remote.zigbee.ensure().endpoints.push_back(remoteEndpoint);
        remote.zigbee.ensure().configuredSignature = gateway.deviceConfigurationSignature(remote);
        NativeRemoteBinding binding; binding.ieeeAddress = remoteIeee; binding.groupId = 2;
        gateway.nativeBindings_.push_back(binding);
        Gateway::NativeBindOperation otherBinding; otherBinding.ieeeAddress = "unrelated-remote";
        gateway.nativeBindOperations_.push_back(otherBinding);
        gateway.registerNode(0xd938, remoteEui, true);
        gateway.interviews_[0xd938].operations.clear(); gateway.interviews_[0xd938].operationIndex = 0;
        gateway.buildConfigurationOperations(0xd938);
        while (gateway.interviews_.count(0xd938)) gateway.completeInterviewOperation(0xd938);
        bool restoredBinding = false, preservedBinding = false;
        for (unsigned long i = 0; i < gateway.nativeBindOperations_.size(); ++i) {
            const Gateway::NativeBindOperation& operation = gateway.nativeBindOperations_[i];
            if (operation.ieeeAddress == remoteIeee && !operation.unbind) restoredBinding = true;
            if (operation.ieeeAddress == "unrelated-remote") preservedBinding = true;
        }
        assert(restoredBinding && preservedBinding);
        assert(gateway.sceneProvisioning_.count("unrelated"));
        unlink((std::string(directory) + "/devices.json").c_str());
        rmdir(directory);
    }
    static void gatewaySettings() {
        char directory[]="/tmp/microhad-quorum-test-XXXXXX"; assert(mkdtemp(directory));
        Gateway gateway("/dev/null",115200,directory,"","","");
        gateway.configureBroadcastQuorum(6);
        gateway.configureMatterCommissioningRegion("GB", "outdoor");
        std::string error;
        assert(gateway.loadGatewaySettings(&error) && gateway.broadcastQuorum_==6);
        assert(gateway.captureView().matterCountryCode == "GB");
        assert(gateway.captureView().matterRegulatoryLocation == "outdoor");
        const std::string legacyPath = std::string(directory) + "/gateway-settings.json";
        FILE* legacy = fopen(legacyPath.c_str(), "w");
        assert(legacy && fputs("{\"broadcast_passive_ack_quorum\":7}", legacy) >= 0);
        assert(fclose(legacy) == 0);
        assert(gateway.loadGatewaySettings(&error) && gateway.broadcastQuorum_ == 7);
        assert(gateway.captureView().matterCountryCode == "GB");
        assert(gateway.captureView().matterRegulatoryLocation == "outdoor");
        assert(unlink(legacyPath.c_str()) == 0);
        gateway.configureBroadcastQuorum(6);
        assert(!gateway.setBroadcastQuorum(0,&error));
        assert(!gateway.setBroadcastQuorum(256,&error));
#if MICROHAD_ZIGGURAT_ENABLED
        gateway.useZiggurat("/tmp/test-quorum.sock");
        ZigguratClient& client=*gateway.ziggurat_;
        int sockets[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
        assert(fcntl(sockets[0],F_SETFL,O_NONBLOCK)==0);
        client.lockFd_=open("/dev/null",O_RDONLY); assert(client.lockFd_>=0);
        client.fd_=sockets[0]; client.hello_=true; client.ready_=true; client.checkpointAt_=LONG_MAX;
        assert(gateway.setBroadcastQuorum(3,&error));
        unsigned int first=gateway.broadcastQuorumRequest_; assert(first);
        ZigguratWire::Bytes wire=client.output_; assert(!wire.empty()); wire.pop_back();
        ZigguratWire::Bytes packet; assert(ZigguratWire::decode(wire,&packet));
        const std::string name="broadcast_passive_ack_quorum";
        assert(packet[0]==0x38 && packet[3]==name.size());
        assert(std::string(packet.begin()+4,packet.begin()+4+name.size())==name);
        assert(packet.size()==4+name.size()+8 && packet[4+name.size()]==3);
        for (unsigned long i=5+name.size();i<packet.size();++i) assert(packet[i]==0);
        assert(gateway.setBroadcastQuorum(4,&error));
        assert(gateway.broadcastQuorumRequest_==first && gateway.sentBroadcastQuorum_==3);
        tunableReply(client,first,0); gateway.pollZiggurat();
        assert(gateway.appliedBroadcastQuorum_==3 && gateway.sentBroadcastQuorum_==4);
        assert(gateway.broadcastQuorumState_=="pending" && gateway.broadcastQuorumRequest_);
        tunableReply(client,gateway.broadcastQuorumRequest_,0); gateway.pollZiggurat();
        assert(gateway.appliedBroadcastQuorum_==4 && gateway.broadcastQuorumState_=="applied");
        std::string path=std::string(directory)+"/gateway-settings.json";
        struct stat before,after; assert(stat(path.c_str(),&before)==0);
        assert(gateway.setBroadcastQuorum(4,&error)); assert(stat(path.c_str(),&after)==0);
        assert(before.st_ino==after.st_ino); // An unchanged value must not rewrite flash.
        tunableReply(client,gateway.broadcastQuorumRequest_,2); gateway.pollZiggurat();
        assert(gateway.broadcastQuorumState_=="unsupported" && gateway.appliedBroadcastQuorum_==4);
        assert(gateway.setBroadcastQuorum(5,&error));
        tunableReply(client,gateway.broadcastQuorumRequest_,3); gateway.pollZiggurat();
        assert(gateway.broadcastQuorumState_=="error" && gateway.appliedBroadcastQuorum_==4);
        assert(gateway.setBroadcastQuorum(6,&error));
        client.pending_[gateway.broadcastQuorumRequest_].expires=1;
        gateway.pollZiggurat(); assert(gateway.broadcastQuorumState_=="error");
        client.disconnect("test disconnect"); gateway.pollZiggurat();
        assert(gateway.appliedBroadcastQuorum_==0 && gateway.broadcastQuorumState_=="pending");
        close(sockets[1]);
        assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
        assert(fcntl(sockets[0],F_SETFL,O_NONBLOCK)==0);
        client.fd_=sockets[0]; client.hello_=true; client.ready_=true; client.checkpointAt_=LONG_MAX;
        client.state_.network=ZigguratWire::Bytes(82,0);
        client.events_.push_back(ZigguratClient::Event(ZigguratClient::Event::Ready));
        gateway.pollZiggurat(); assert(gateway.sentBroadcastQuorum_==6 && gateway.broadcastQuorumRequest_);
        tunableReply(client,gateway.broadcastQuorumRequest_,0); gateway.pollZiggurat();
        assert(gateway.captureView().settingsJson.find("\"applied_value\":6")!=std::string::npos);
        assert(gateway.setMatterCommissioningRegion("US", "outdoor", &error));
        assert(!gateway.setMatterCommissioningRegion("us", "outdoor", &error));
        assert(!gateway.setMatterCommissioningRegion("US", "outside", &error));
        assert(gateway.captureView().matterCountryCode == "US" && gateway.captureView().matterRegulatoryLocation == "outdoor");
        client.disconnect("test cleanup"); close(sockets[1]);
        Gateway restored("/dev/null",115200,directory,"","","");
        restored.configureMatterCommissioningRegion("JP", "indoor");
        restored.configureBroadcastQuorum(8);
        assert(restored.loadGatewaySettings(&error) && restored.broadcastQuorum_==6);
        assert(restored.captureView().matterCountryCode == "US" && restored.captureView().matterRegulatoryLocation == "outdoor");
        assert(unlink(path.c_str())==0); assert(mkdir(path.c_str(),0700)==0);
        assert(!gateway.setBroadcastQuorum(7,&error) && gateway.broadcastQuorum_==6);
        assert(rmdir(path.c_str())==0);
        std::ofstream invalid(path.c_str()); invalid << "{\"broadcast_passive_ack_quorum\":0}"; invalid.close();
        assert(!restored.loadGatewaySettings(&error)); assert(unlink(path.c_str())==0);
#else
        assert(gateway.captureView().settingsJson.find("unsupported")!=std::string::npos);
        assert(!gateway.setBroadcastQuorum(3,&error));
#endif
        assert(rmdir(directory)==0);
    }
#if MICROHAD_ZIGGURAT_ENABLED
    static void tunableReply(ZigguratClient& client,unsigned int id,unsigned char status) {
        ZigguratWire::Bytes reply(1,0x38); ZigguratWire::put16(&reply,(id<<2)|1); reply.push_back(status);
        client.frame(reply);
    }
#endif
    static void restoreState(Gateway& gateway) { gateway.loadState(); }
    static void observedChannelSnapshot() {
        char valueHistory[] = "/tmp/microhad-matter-values-test-XXXXXX";
        int valueHistoryFd = mkstemp(valueHistory);
        assert(valueHistoryFd >= 0);
        close(valueHistoryFd);
        unlink(valueHistory);
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "", 0, valueHistory);
        gateway.online_ = true;
        gateway.networkFormed_ = true;
        gateway.networkChannel_ = 20; /* Persisted configuration is not an observation. */
        gateway.observedNetworkChannel_ = 0;
        assert(gateway.captureView().observedNetworkChannel == 0);
        gateway.observedNetworkChannel_ = 15;
        GatewayView first = gateway.captureView();
        assert(first.observedNetworkChannel == 15);
        assert(first.statusJson().find("\"observed_network_channel\":15") != std::string::npos);
        gateway.observedNetworkChannel_ = 26;
        assert(first.observedNetworkChannel == 15);
        assert(gateway.captureView().observedNetworkChannel == 26);
        gateway.online_ = false;
        assert(gateway.captureView().observedNetworkChannel == 0);
        gateway.online_ = true;
        gateway.networkFormed_ = false;
        assert(gateway.captureView().observedNetworkChannel == 0);

        GatewayView sensors;
        Device matter;
        matter.source = "matter";
        matter.external.ensure().externalId = "matter:0000000000000001:0000000000000002:0001";
        matter.airQuality.setPresent(true);
        matter.airQuality.ensure().known = true;
        matter.airQuality.ensure().value = "moderate";
        matter.co2.setPresent(true);
        matter.co2.ensure().known = true;
        matter.co2.ensure().ppm = 758.5;
        matter.co2.ensure().unit = "ppb";
        matter.pm25.setPresent(true);
        matter.pm25.ensure().known = true;
        matter.pm25.ensure().microgramsM3 = 12.75;
        matter.pm25.ensure().unit = "ug/m3";
        sensors.devices.push_back(matter);
        const std::string sensorJson = sensors.statusJson();
        assert(sensorJson.find("\"has_air_quality\":true,\"air_quality_known\":true,"
                               "\"air_quality\":\"moderate\"") != std::string::npos);
        assert(sensorJson.find("\"has_co2\":true,\"co2_known\":false,\"co2_ppm\":0") !=
               std::string::npos);
        assert(sensorJson.find("\"has_pm25\":true,\"pm25_known\":true,"
                               "\"pm25_micrograms_m3\":12.75") != std::string::npos);
        assert(sensorJson.find("\"co2\":{\"value\":758.5,\"unit\":\"ppb\",\"quality\":\"measured\"}") !=
               std::string::npos);

        MatterEndpoint endpoint;
        endpoint.id = "matter:0000000000000001:0000000000000002:0001";
        endpoint.node = "0000000000000002";
        endpoint.endpoint = 1;
        endpoint.online = true;
        endpoint.servers.push_back(6);
        endpoint.servers.push_back(8);
        endpoint.servers.push_back(29);
        endpoint.servers.push_back(1026);
        endpoint.servers.push_back(1037);
        endpoint.servers.push_back(69);
        std::vector<MatterEndpoint> endpoints(1, endpoint);
        std::string error;
        assert(gateway.reconcileMatterDevices(endpoints, &error));
        const std::string matterStatus = gateway.statusJson();
        assert(matterStatus.find("\"id\":\"matter:0000000000000001:0000000000000002:0001\"") !=
               std::string::npos);
        assert(matterStatus.find("\"matter_node\":\"0000000000000002\",\"matter_endpoint\":1") !=
               std::string::npos);
        assert(matterStatus.find("\"has_co2\":true,\"co2_known\":false") != std::string::npos);
        assert(matterStatus.find("\"has_contact\":true") != std::string::npos);
        MatterReading contact;
        contact.node = endpoint.node;
        contact.endpoint = endpoint.endpoint;
        contact.cluster = 69;
        contact.known = true;
        contact.booleanValue = true;
        assert(gateway.applyMatterReading(contact, &error));
        assert(gateway.statusJson().find("\"contact_known\":true,\"contact_open\":false") != std::string::npos);
        contact.booleanValue = false;
        assert(gateway.applyMatterReading(contact, &error));
        assert(gateway.statusJson().find("\"contact_known\":true,\"contact_open\":true") != std::string::npos);
        MatterReading co2;
        co2.node = endpoint.node;
        co2.endpoint = endpoint.endpoint;
        co2.cluster = 1037;
        co2.known = true;
        co2.numberValue = 810.5;
        co2.unit = "ppb";
        assert(gateway.applyMatterReading(co2, &error));
        assert(gateway.statusJson().find("\"co2\":{\"value\":810.5,\"unit\":\"ppb\",\"quality\":\"measured\"}") !=
               std::string::npos);
        co2.known = false;
        assert(gateway.applyMatterReading(co2, &error));
        assert(gateway.statusJson().find("\"co2\":{\"value\":null,\"unit\":\"ppb\",\"quality\":\"unknown\"}") !=
               std::string::npos);
        std::vector<MatterEndpoint> staleEndpoints = endpoints;
        staleEndpoints[0].online = false;
        assert(gateway.reconcileMatterDevices(staleEndpoints, &error));
        assert(gateway.statusJson().find("\"co2\":{\"value\":null,\"unit\":\"ppb\",\"quality\":\"unknown\"}") !=
               std::string::npos);
        assert(gateway.devices_[endpoint.id].online && gateway.devices_[endpoint.id].lastSeen > 0);
        gateway.devices_[endpoint.id].lastSeen = 1;
        assert(gateway.reconcileMatterDevices(staleEndpoints, &error));
        assert(!gateway.devices_[endpoint.id].online && gateway.devices_[endpoint.id].lastSeen == 0);
        DeviceValueAlert temperatureAlert;
        temperatureAlert.hasHigh = true;
        temperatureAlert.high = 25;
        gateway.devices_[endpoint.id].valueAlerts["temperature"] = temperatureAlert;
        MatterReading temperature;
        temperature.node = endpoint.node;
        temperature.endpoint = endpoint.endpoint;
        temperature.cluster = 1026;
        temperature.known = true;
        temperature.numberValue = 20;
        assert(gateway.applyMatterReading(temperature, &error));
        temperature.numberValue = 26;
        assert(gateway.applyMatterReading(temperature, &error));
        assert(gateway.devices_[endpoint.id].valueAlerts["temperature"].highActive);
        assert(gateway.reconcileMatterDevices(endpoints, &error));
        assert(gateway.devices_[endpoint.id].valueAlerts["temperature"].highActive);
        unlink((std::string(valueHistory) + ".values").c_str());
    }
    static void ineligibleSensorRefreshCannotSpin() {
        char directory[] = "/tmp/microhad-refresh-timers-XXXXXX";
        assert(mkdtemp(directory) != 0);
        {
            Gateway gateway("/dev/null", 115200, directory, "", "", "");
            const unsigned short contactNode = 0x86fe;
            const unsigned short sensorNode = 0x77e1;
            const std::string contactIeee = "0x00124b00292bfbd8";
            const std::string sensorIeee = "0x00124b0000000002";
            Device contact;
            contact.ieeeAddress = contactIeee;
            contact.zigbee.ensure().networkAddress = contactNode;
            contact.contact.setPresent(true);
            contact.sensorRefreshIntervalMinutes = 5;
            gateway.devices_[contactIeee] = contact;
            gateway.nodeAddresses_[contactNode] = contactIeee;
            Device sensor;
            sensor.ieeeAddress = sensorIeee;
            sensor.zigbee.ensure().networkAddress = sensorNode;
            EndpointDescriptor endpoint;
            endpoint.inputClusters.push_back(0x0402);
            sensor.zigbee.ensure().endpoints.push_back(endpoint);
            gateway.devices_[sensorIeee] = sensor;
            gateway.nodeAddresses_[sensorNode] = sensorIeee;
            const long now = time(0);
            gateway.online_ = true;
            gateway.networkFormed_ = true;
            gateway.nextHeartbeatAt_ = now + 20;
            gateway.nextSensorRefreshAt_[contactNode] = now - 3600;
            gateway.nextSensorRefreshAt_[sensorNode] = now + 60;
            gateway.nextSensorRefreshAt_[0x9999] = now - 1; /* orphan */
            std::string source;
            assert(gateway.nextPollDeadline(&source) == now + 20);
            assert(source == "heartbeat");
            gateway.poll();
            assert(gateway.nextSensorRefreshAt_.count(contactNode) == 0);
            assert(gateway.nextSensorRefreshAt_.count(0x9999) == 0);
            assert(gateway.nextSensorRefreshAt_[sensorNode] == now + 60);
            assert(logOccurrences(gateway, "Reset inactive deadline: source=sensor_refresh") == 2);
            gateway.poll();
            assert(logOccurrences(gateway, "Reset inactive deadline: source=sensor_refresh") == 2);

            /* Announcements and manual refreshes must not recreate the contact timer. */
            const unsigned char eui64[] = {0xd8, 0xfb, 0x2b, 0x29, 0x00, 0x4b, 0x12, 0x00};
            gateway.registerNode(contactNode, eui64);
            assert(gateway.nextSensorRefreshAt_.count(contactNode) == 0);
            gateway.interviews_.clear();
            gateway.interviewRequestInFlight_ = false;
            gateway.interviewDeadline_ = 0;
            std::string error;
            assert(gateway.refreshDevice(contactIeee, &error));
            assert(gateway.nextSensorRefreshAt_.count(contactNode) == 0);
            gateway.interviews_.clear();
            gateway.interviewRequestInFlight_ = false;
            gateway.interviewDeadline_ = 0;

            gateway.nextHeartbeatAt_ = now + 120;
            assert(gateway.nextPollDeadline(&source) == now + 60);
            assert(source == "sensor_refresh node=0x77e1");
            gateway.devices_[sensorIeee].sensorRefreshIntervalMinutes = 0;
            gateway.nextSensorRefreshAt_[sensorNode] = now - 1;
            assert(gateway.nextPollDeadline(&source) == now + 120);
            gateway.poll();
            assert(gateway.nextSensorRefreshAt_.empty());
            gateway.devices_[sensorIeee].sensorRefreshIntervalMinutes = 60;
            gateway.nextSensorRefreshAt_[sensorNode] = now - 1;
            gateway.devices_[sensorIeee].zigbee.ensure().networkAddress = 0x1234; /* stale mapping */
            assert(gateway.nextPollDeadline(&source) == now + 120);
        }
        unlink((std::string(directory) + "/devices.json").c_str());
        unlink((std::string(directory) + "/devices-state.json").c_str());
        assert(rmdir(directory) == 0);
    }
    static void emptyDeadlineIsUnscheduled() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        std::string source;
        assert(gateway.nextPollDeadline(&source) == 0);
        assert(source == "none");
    }
    static void inactiveDeadlinesAreResetBeforeSelection() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        const long now = time(0);
        gateway.online_ = true;
        gateway.networkFormed_ = true;
        gateway.nextHeartbeatAt_ = now + 20;
        gateway.ashAckDeadline_ = now - 1;
        gateway.resetDeadline_ = now - 1;
        gateway.coordinatorInitializationDeadline_ = now - 1;
        gateway.networkFormationDeadline_ = now - 1;
        gateway.interviewDeadline_ = now - 1;
        gateway.joinPreparationDeadline_ = now - 1;
        gateway.joinPermitCloseDeadline_ = now - 1;
        gateway.joinPermitRenewAt_ = now - 1;
        Device contact;
        contact.contact.setPresent(true);
        gateway.devices_["closed"] = contact;
        contact.contact.ensure().open = true;
        contact.contactTriggerMode = true;
        gateway.devices_["trigger"] = contact;
        gateway.nextContactAlertAt_["closed"] = now - 1;
        gateway.nextContactAlertAt_["missing"] = now - 1;
        gateway.nextContactAlertAt_["trigger"] = now - 1;
        Gateway::InterviewState paused;
        paused.paused = true;
        Gateway::InterviewOperation operation;
        operation.retryAt = now - 1;
        paused.operations.push_back(operation);
        gateway.interviews_[0x1234] = paused;
        Gateway::SceneProvisionState pending;
        pending.deadline = now - 1;
        pending.waitingForTraffic = true;
        gateway.sceneProvisioning_["missing"] = pending;
        Gateway::SceneRetry retry;
        retry.retryAt = now - 1;
        gateway.sceneRetries_[123] = retry;
        std::string source;
        assert(gateway.nextPollDeadline(&source) == now + 20);
        assert(source == "heartbeat");
        assert(gateway.ashAckDeadline_ == 0 && gateway.resetDeadline_ == 0);
        assert(gateway.coordinatorInitializationDeadline_ == 0 && gateway.networkFormationDeadline_ == 0);
        assert(gateway.interviewDeadline_ == 0 && gateway.joinPreparationDeadline_ == 0);
        assert(gateway.joinPermitCloseDeadline_ == 0 && gateway.joinPermitRenewAt_ == 0);
        assert(gateway.nextContactAlertAt_.empty());
        assert(gateway.interviews_[0x1234].operations[0].retryAt == 0);
        assert(gateway.sceneProvisioning_["missing"].deadline == 0);
        assert(gateway.sceneRetries_.empty());
        unsigned long resets = logOccurrences(gateway, "Reset inactive deadline:");
        gateway.nextPollDeadline();
        assert(logOccurrences(gateway, "Reset inactive deadline:") == resets);

        /* Backpressure suspends the ACK timer without discarding its frame. */
        gateway.ashPendingData_.push_back(1);
        gateway.serialPendingBytes_.push_back(2);
        gateway.ashAckDeadline_ = now - 1;
        assert(gateway.nextPollDeadline(&source) == now + 20);
        assert(gateway.ashAckDeadline_ == 0 && !gateway.ashPendingData_.empty());
        gateway.serialPendingBytes_.clear();
        gateway.pollAshTransmit();
        assert(gateway.ashAckDeadline_ >= now + 2);
        assert(gateway.ashRetries_ == 0);
        assert(gateway.nextPollDeadline(&source) == gateway.ashAckDeadline_);
        assert(source == "ash_ack");
    }
    static void pausedRefreshWaitsForScheduledWake() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        const unsigned short node = 0x1234;
        const std::string ieee = "0x00124b0000000011";
        Gateway::InterviewState state;
        Gateway::InterviewOperation operation;
        state.ieeeAddress = ieee;
        state.refreshOnly = true;
        state.paused = true;
        state.operations.push_back(operation);
        gateway.interviews_[node] = state;
        gateway.devices_[ieee].ieeeAddress = ieee;
        gateway.nodeAddresses_[node] = ieee;
        gateway.wakeInterview(node);
        assert(gateway.interviews_[node].paused);
        gateway.startInterview(node, true);
        assert(!gateway.interviews_[node].paused);
    }
    static void cachedRejoinConfiguresReporting() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        const unsigned short node = 0x1234;
        const std::string ieee = "0x00124b002a52140a";
        EndpointDescriptor endpoint;
        endpoint.endpoint = 1;
        endpoint.inputClusters.push_back(0x0402);
        gateway.devices_[ieee].ieeeAddress = ieee;
        gateway.devices_[ieee].zigbee.ensure().endpoints.push_back(endpoint);
        gateway.nodeAddresses_[node] = ieee;

        gateway.startInterview(node, false);
        assert(!gateway.interviews_[node].refreshOnly);
        assert(gateway.interviews_[node].operations.size() == 3);
        assert(gateway.interviews_[node].operations[0].type == Gateway::InterviewBind);
        assert(gateway.interviews_[node].operations[1].type == Gateway::InterviewConfigureReporting);
        gateway.interviews_.clear();

        /* Ordinary refreshes remain reads; a rejoin upgrades only after the
           existing request completes, preserving its sequence and deadline. */
        gateway.startInterview(node, true);
        assert(gateway.interviews_[node].operations.size() == 1);
        Gateway::InterviewOperation& operation = gateway.interviews_[node].operations[0];
        assert(operation.type == Gateway::InterviewAttributeRead);
        operation.transactionSequence = 42;
        operation.transactionSequenceAssigned = true;
        gateway.interviewRequestInFlight_ = true;
        gateway.interviewNode_ = node;
        gateway.interviewDeadline_ = time(0) + 20;
        gateway.interviews_[node].paused = true;
        gateway.startInterview(node, false);
        assert(!gateway.interviews_[node].paused && gateway.interviews_[node].configurationPending);
        assert(gateway.interviewRequestInFlight_ && gateway.interviewDeadline_ != 0);
        assert(operation.type == Gateway::InterviewAttributeRead && operation.transactionSequence == 42);
        gateway.interviews_[node].paused = true;
        gateway.wakeInterview(node);
        assert(!gateway.interviews_[node].paused);
        gateway.completeInterviewOperation(node);
        assert(!gateway.interviewRequestInFlight_);
        assert(!gateway.interviews_[node].refreshOnly && !gateway.interviews_[node].configurationPending);
        assert(gateway.interviews_[node].operations[0].type == Gateway::InterviewBind);
        assert(gateway.interviews_[node].operations[1].type == Gateway::InterviewConfigureReporting);
        gateway.interviews_.clear();

        gateway.startInterview(node, true);
        gateway.completeInterviewOperation(node);
        assert(gateway.interviews_.empty());
    }
    static void deadlineSelectionMatchesHandlerOrdering() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        const long now = time(0);
        gateway.online_ = true;
        gateway.networkFormed_ = true;
        gateway.nextHeartbeatAt_ = now + 20;
        Gateway::NativeBindOperation first;
        first.deadline = now + 10;
        gateway.nativeBindOperations_.push_back(first);
        Gateway::NativeBindOperation second;
        second.deadline = now - 1;
        gateway.nativeBindOperations_.push_back(second);
        std::string source;
        assert(gateway.nextPollDeadline(&source) == now + 10);
        assert(source == "native_bind");
        gateway.interviewRequestInFlight_ = true;
        gateway.interviewDeadline_ = now + 5;
        assert(gateway.nextPollDeadline(&source) == now + 5);
        assert(source == "interview");
        assert(gateway.nativeBindOperations_[1].deadline == now - 1); /* valid work is retained */
        gateway.interviewRequestInFlight_ = false;
        gateway.nativeBindOperations_.clear();
        Gateway::SceneProvisionState pending;
        Gateway::SceneProvisionCommand command;
        pending.commands.push_back(command);
        pending.deadline = now + 10;
        gateway.sceneProvisioning_["a"] = pending;
        pending.deadline = now - 1;
        gateway.sceneProvisioning_["b"] = pending;
        gateway.devices_["a"].zigbee.ensure().networkAddress = 1;
        gateway.devices_["b"].zigbee.ensure().networkAddress = 2;
        assert(gateway.nextPollDeadline(&source) == now + 10);
        assert(source == "scene_provisioning");
        gateway.coordinatorInitializationStep_ = 1;
        gateway.coordinatorInitializationDeadline_ = now + 5;
        assert(gateway.nextPollDeadline(&source) == now + 5);
        assert(gateway.sceneProvisioning_["b"].deadline == now - 1);
        gateway.coordinatorInitializationStep_ = 0;
        gateway.sceneProvisioning_["a"].deadline = 0;
        assert(gateway.nextPollDeadline(&source) == time(0));
        assert(source == "scene_provisioning");
    }
    static void pausedTimersDoNotWinDeadline() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        std::string source;
        long now = time(0);
        Gateway::NativeBindOperation operation;
        operation.deadline = now - 1;
        gateway.nativeBindOperations_.push_back(operation);
        gateway.online_ = true;
        gateway.networkFormed_ = true;
        gateway.interviewRequestInFlight_ = true;
        gateway.interviewDeadline_ = now + 10;
        assert(gateway.nextPollDeadline(&source) >= now + 9);
        assert(source == "interview");

        gateway.interviewRequestInFlight_ = false;
        gateway.coordinatorInitializationStep_ = 1;
        gateway.coordinatorInitializationDeadline_ = now + 10;
        Gateway::SceneProvisionState pending;
        pending.deadline = now - 1;
        gateway.sceneProvisioning_["device"] = pending;
        assert(gateway.nextPollDeadline(&source) >= now + 9);
        assert(source == "coordinator_initialization");
    }
    static bool hasContactReminder(const Gateway& gateway, const std::string& ieeeAddress) {
        return gateway.nextContactAlertAt_.count(ieeeAddress) != 0;
    }
    static bool contactTriggerStatusExpires(Gateway* gateway, const std::string& ieeeAddress) {
        gateway->nextContactTriggerAt_[ieeeAddress] = time(0) + 30;
        if (gateway->statusJson().find("\"contact_trigger_active\":true") == std::string::npos) return false;
        gateway->nextContactTriggerAt_[ieeeAddress] = time(0) - 1;
        return gateway->statusJson().find("\"contact_trigger_active\":false") != std::string::npos;
    }
    static bool contactAlertDeadlines(Gateway* gateway, const std::string& ieeeAddress) {
        std::vector<unsigned char> open(2, 0);
        open[0] = 1;
        if (!gateway->applyIasZoneStatus(ieeeAddress, open)) return false;
        long firstDeadline = gateway->nextContactAlertAt_[ieeeAddress];
        if (!gateway->applyIasZoneStatus(ieeeAddress, open) || gateway->nextContactAlertAt_[ieeeAddress] != firstDeadline)
            return false;
        gateway->nextContactAlertAt_[ieeeAddress] = time(0) - 1;
        gateway->poll();
        long reminderDeadline = gateway->nextContactAlertAt_[ieeeAddress];
        if (reminderDeadline <= time(0)) return false;
        gateway->nextContactAlertAt_[ieeeAddress] = time(0) - 1;
        gateway->poll();
        return true;
    }
    static unsigned long logOccurrences(const Gateway& gateway, const std::string& text) {
        std::string logs = gateway.logsJson();
        std::string::size_type at = 0;
        unsigned long count = 0;
        while ((at = logs.find(text, at)) != std::string::npos) { ++count; at += text.size(); }
        return count;
    }
    static void completeViewSchema(Gateway* gateway) {
        Device device;
        EndpointDescriptor endpoint;
        device.ieeeAddress = "0xview";
        device.name = "Kitchen \"window\"\n";
        device.kind = "temperature/humidity";
        device.contact.setPresent(true); device.contact.ensure().open = true;
        device.temperature.setPresent(true); device.temperature.ensure().celsius = 21.5;
        device.humidity.setPresent(true); device.humidity.ensure().percent = 40.5;
        device.occupancy.setPresent(true); device.occupancy.ensure().occupied = true;
        device.battery.setPresent(true); device.battery.ensure().percent = 80;
        device.light.setPresent(true); device.light.ensure().on = true; device.brightness.setPresent(true); device.brightness.ensure().percent = 50;
        device.remote.setPresent(true); device.hasLowTemperatureAlert = true; device.lowTemperatureAlert = 5;
        device.hasHighTemperatureAlert = true; device.highTemperatureAlert = 30;
        device.manufacturerName = "Acme\t"; device.modelId = "Model\\1";
        endpoint.endpoint = 2; endpoint.profileId = 0x0104; endpoint.deviceId = 0x0101;
        endpoint.inputClusters.push_back(0x0006); endpoint.outputClusters.push_back(0x0008);
        device.zigbee.ensure().endpoints.push_back(endpoint);
        gateway->devices_[device.ieeeAddress] = device;
        gateway->addLog("view control\n\001");
    }
    static bool contactAlertIsCancelled(Gateway* gateway, const std::string& ieeeAddress) {
        std::vector<unsigned char> open;
        std::vector<unsigned char> closed;
        open.push_back(1); open.push_back(0);
        closed.push_back(0); closed.push_back(0);
        if (!gateway->applyIasZoneStatus(ieeeAddress, open) ||
            gateway->nextContactAlertAt_.count(ieeeAddress) != 1 ||
            gateway->contactOpenedAt_.count(ieeeAddress) != 1) return false;
        if (!gateway->applyIasZoneStatus(ieeeAddress, closed) ||
            gateway->nextContactAlertAt_.count(ieeeAddress) != 0 ||
            gateway->contactOpenedAt_.count(ieeeAddress) != 0) return false;
        gateway->poll();
        return gateway->logsJson().find("Notification queued: Window open") == std::string::npos &&
               gateway->logsJson().find("Notification suppressed: Window open") == std::string::npos;
    }
    static void logRingAndSerialization() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        for (int i = 0; i < 1400; ++i) {
            std::ostringstream entry;
            entry << "entry-" << i << std::string(760, 'a');
            gateway.addLog(entry.str());
        }
        std::string logs = gateway.logsJson();
        assert(logs.find("entry-0") == std::string::npos);
        assert(logs.find("entry-1399") != std::string::npos);
        gateway.addLog(std::string(800, 'x'));
        gateway.addLog("control\n\t\001");
        logs = gateway.logsJson();
        assert(logs.find("\\n\\t\\u0001") != std::string::npos);
        assert(logs.find(std::string(769, 'x')) == std::string::npos);
        assert(gateway.deviceLogLabel(0x1234) == "unknown node=0x1234");
    }
    static std::vector<unsigned char> decodedLastEzsp(const Gateway& gateway) {
        std::vector<unsigned char> command = gateway.ashTransmitQueue_.empty() ? gateway.ashPendingData_ : gateway.ashTransmitQueue_.back();
        unsigned char random = 0x42;
        for (unsigned long i = 0; i < command.size(); ++i) {
            command[i] ^= random;
            random = (random & 1) ? static_cast<unsigned char>((random >> 1) ^ 0xb8) :
                                   static_cast<unsigned char>(random >> 1);
        }
        return command;
    }
    static void closingPermitReplyTimeout() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        std::string error;
        gateway.online_ = true;
        gateway.networkFormed_ = true;
        gateway.ezspVersion_ = 8;
        gateway.joinUntil_ = time(0) + 120;
        gateway.joinPermitRenewAt_ = time(0) + 100;
        assert(gateway.setJoining(0, &error));
        assert(gateway.joinPermitClosePending_);
        assert(gateway.joinPermitCloseDeadline_ >= time(0) + 9);
        gateway.joinPermitCloseDeadline_ = time(0) - 1;
        assert(gateway.nextPollDeadline() == time(0));
        gateway.poll();
        assert(!gateway.joinPermitClosePending_ && gateway.joinPermitCloseDeadline_ == 0);
        assert(gateway.joinUntil_ == 0 && gateway.joinPermitRenewAt_ == 0);
        assert(gateway.statusJson().find("\"joining_until\":0") != std::string::npos);
        std::vector<unsigned char> retry = decodedLastEzsp(gateway);
        assert(retry[3] == 0x22 && retry[5] == 0);
    }
    static void refreshDiscoversMissingEndpoints() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        Device device;
        std::string error;
        device.ieeeAddress = "0x00124b00000000dd";
        device.zigbee.ensure().networkAddress = 0x4321;
        gateway.devices_[device.ieeeAddress] = device;
        gateway.nodeAddresses_[device.zigbee.ensure().networkAddress] = device.ieeeAddress;
        gateway.online_ = true;
        gateway.networkFormed_ = true;
        assert(gateway.refreshDevice(device.ieeeAddress, &error));
        assert(gateway.interviews_[device.zigbee.read().networkAddress].refreshOnly);
        assert(gateway.interviews_[device.zigbee.read().networkAddress].operations[0].type == Gateway::InterviewActiveEndpoints);
    }
    static void lightCommandEncoding() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        Device device;
        EndpointDescriptor power;
        EndpointDescriptor level;
        std::string error;
        device.ieeeAddress = "0x00124b00000000cc";
        device.zigbee.ensure().networkAddress = 0x4321;
        device.light.setPresent(true);
        device.light.ensure().on = false;
        power.endpoint = 3; power.inputClusters.push_back(0x0006);
        level.endpoint = 7; level.inputClusters.push_back(0x0008);
        device.zigbee.ensure().endpoints.push_back(power);
        device.zigbee.ensure().endpoints.push_back(level);
        gateway.devices_[device.ieeeAddress] = device;
        gateway.online_ = true; gateway.networkFormed_ = true; gateway.coordinatorInitializationStep_ = 0; gateway.ezspVersion_ = 8;

        assert(gateway.setLightState(device.ieeeAddress, "on", -1, &error));
        std::vector<unsigned char> command = decodedLastEzsp(gateway);
        assert(command.size() == 24 && command[3] == 0x34 && command[4] == 0x00);
        assert(command[6] == 0x21 && command[7] == 0x43 && command[8] == 0x04 && command[9] == 0x01);
        assert(command[10] == 0x06 && command[11] == 0x00 && command[12] == 1 && command[13] == 3);
        assert(command[20] == 3 && command[21] == 0x11 && command[22] == 1 && command[23] == 1);

        assert(gateway.setLightState(device.ieeeAddress, "off", -1, &error));
        command = decodedLastEzsp(gateway);
        assert(command[12] == 1 && command[13] == 3 && command[20] == 3 && command[23] == 0);
        assert(gateway.setLightState(device.ieeeAddress, "toggle", -1, &error));
        command = decodedLastEzsp(gateway);
        assert(command[23] == 2);

        assert(gateway.setLightState(device.ieeeAddress, "ignored", 50, &error));
        command = decodedLastEzsp(gateway);
        assert(command.size() == 27 && command[10] == 0x08 && command[11] == 0 && command[12] == 1 && command[13] == 7);
        assert(command[20] == 6 && command[21] == 0x11 && command[23] == 0x04 &&
               command[24] == 127 && command[25] == 0 && command[26] == 0);

        assert(!gateway.setLightState("0xmissing", "on", -1, &error));
        assert(error == "device is not a light");
        Device nondimmable = device;
        nondimmable.ieeeAddress = "0x00124b00000000cd";
        nondimmable.zigbee.ensure().endpoints.erase(nondimmable.zigbee.ensure().endpoints.begin() + 1);
        gateway.devices_[nondimmable.ieeeAddress] = nondimmable;
        assert(!gateway.setLightState(nondimmable.ieeeAddress, "on", 1, &error));
        assert(error == "brightness is unavailable for this light");
        assert(!gateway.setLightState(nondimmable.ieeeAddress, "invalid", -1, &error));
        assert(error == "invalid light state");
        Device unsupported = device;
        unsupported.ieeeAddress = "0x00124b00000000ce";
        unsupported.zigbee.ensure().endpoints.clear();
        gateway.devices_[unsupported.ieeeAddress] = unsupported;
        assert(!gateway.setLightState(unsupported.ieeeAddress, "on", -1, &error));
        assert(error == "power control is unavailable for this light");
    }
    static void identifyCommandEncoding() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        Device device;
        EndpointDescriptor clientOnly;
        EndpointDescriptor identify;
        std::string error;
        device.ieeeAddress = "0x00124b00000000ee";
        device.zigbee.ensure().networkAddress = 0x4321;
        clientOnly.endpoint = 1;
        clientOnly.outputClusters.push_back(0x0003);
        identify.endpoint = 4;
        identify.profileId = 0x0104;
        identify.inputClusters.push_back(0x0003);
        device.zigbee.ensure().endpoints.push_back(clientOnly);
        device.zigbee.ensure().endpoints.push_back(identify);
        gateway.devices_[device.ieeeAddress] = device;
        assert(!gateway.identifyDevice(device.ieeeAddress, &error));
        assert(error == "Zigbee network is not ready");
        gateway.online_ = true; gateway.networkFormed_ = true; gateway.ezspVersion_ = 8;
        assert(gateway.identifyDevice(device.ieeeAddress, &error));
        std::vector<unsigned char> command = decodedLastEzsp(gateway);
        assert(command.size() == 26 && command[3] == 0x34 && command[4] == 0x00);
        assert(command[6] == 0x21 && command[7] == 0x43);
        assert(command[8] == 0x04 && command[9] == 0x01); /* endpoint profile */
        assert(command[10] == 0x03 && command[11] == 0x00);
        assert(command[12] == 1 && command[13] == 4);
        assert(command[20] == 5 && command[21] == 0x11 && command[23] == 0x00);
        assert(command[24] == 5 && command[25] == 0);
        assert(!gateway.identifyDevice("missing", &error) && error == "device is unknown");
        gateway.devices_[device.ieeeAddress].zigbee.ensure().networkAddress = 0xffff;
        assert(!gateway.identifyDevice(device.ieeeAddress, &error) && error == "device has no active network address");
        gateway.devices_[device.ieeeAddress].zigbee.ensure().networkAddress = 0x4321;
        gateway.devices_[device.ieeeAddress].zigbee.ensure().endpoints.erase(gateway.devices_[device.ieeeAddress].zigbee.ensure().endpoints.begin() + 1);
        assert(!gateway.identifyDevice(device.ieeeAddress, &error) && error == "device has no Identify server endpoint");
        gateway.devices_[device.ieeeAddress].source = "matter";
        gateway.devices_[device.ieeeAddress].zigbee.ensure().networkAddress = 0xffff;
        assert(!gateway.identifyDevice(device.ieeeAddress, &error));
        assert(error == "Identify is not supported for this device source");
        gateway.online_ = false;
        assert(!gateway.identifyDevice(device.ieeeAddress, &error));
        assert(error == "Identify is not supported for this device source");
    }
    static void ezspUpgradeCompatibility() {
        for (int protocol = 8; protocol <= 13; protocol += 5) {
            Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
            unsigned char legacy[] = {0, 0x80, 0, static_cast<unsigned char>(protocol), 2, 0x80, 0x67};
            gateway.handleEzspFrame(std::vector<unsigned char>(legacy, legacy + sizeof(legacy)));
            assert(gateway.ezspVersion_ == protocol && !gateway.online_);
            std::vector<unsigned char> reply(9, 0);
            reply[1] = 0x80; reply[2] = 1; reply[5] = protocol; reply[6] = 2;
            reply[7] = 0x80; reply[8] = 0x67;
            gateway.handleEzspFrame(reply);
            assert(gateway.online_ && gateway.coordinatorInitializationStep_ == 1);
            /* Required initialization must fail closed on missing status. */
            reply.resize(5); reply[3] = 0x53;
            gateway.handleEzspFrame(reply);
            assert(!gateway.online_ && gateway.coordinatorInitializationStep_ == 1);
            gateway.online_ = true;
            gateway.coordinatorInitializationStep_ = 11;
            reply.resize(6); reply[5] = 0x37;
            gateway.handleEzspFrame(reply);
            assert(gateway.online_ == (protocol == 13));
            assert(gateway.coordinatorInitializationStep_ == (protocol == 13 ? 12 : 11));
            gateway.coordinatorInitializationStep_ = 0;
            gateway.online_ = true; gateway.networkFormed_ = true;
            gateway.ashTransmitQueue_.clear(); gateway.ashTransmitSensitiveQueue_.clear(); gateway.ashPendingData_.clear();
            gateway.joinPreparationStep_ = 2; gateway.pendingJoinSeconds_ = 60;
            reply.resize(6); reply[3] = 0x55; reply[5] = 0;
            gateway.handleEzspFrame(reply);
            assert(gateway.joinPreparationStep_ == 3);
            assert(gateway.ashTransmitQueue_.size() == 1);
            std::vector<unsigned char> request = gateway.ashTransmitQueue_.back();
            unsigned char mask = 0x42;
            for (unsigned long i = 0; i < request.size(); ++i) {
                request[i] ^= mask;
                mask = (mask & 1) ? static_cast<unsigned char>((mask >> 1) ^ 0xb8) :
                                   static_cast<unsigned char>(mask >> 1);
            }
            assert(request[3] == (protocol == 13 ? 0x11 : 0xaf));
            assert(request[4] == (protocol == 13 ? 1 : 0));
            assert(request.size() == static_cast<unsigned long>(protocol == 13 ? 30 : 29));
            if (protocol == 13) assert(request[29] == 0);
            reply[3] = request[3]; reply[4] = request[4];
            /* A v13 error with a zero low byte is still an error. */
            if (protocol == 13) { reply.resize(9, 0); reply[6] = 1; }
            else reply[5] = 1;
            gateway.handleEzspFrame(reply);
            assert(gateway.joinPreparationStep_ == 0 && gateway.joinUntil_ == 0);
            gateway.joinPreparationStep_ = 3; gateway.pendingJoinSeconds_ = 60;
            for (unsigned long i = 5; i < reply.size(); ++i) reply[i] = 0;
            gateway.handleEzspFrame(reply);
            assert(gateway.joinPreparationStep_ == 4);
            if (protocol == 13) {
                gateway.joinPreparationStep_ = 3;
                reply.resize(6);
                gateway.handleEzspFrame(reply);
                assert(gateway.joinPreparationStep_ == 0);
            }
            gateway.joinPreparationStep_ = 3;
            gateway.joinPreparationDeadline_ = time(0) - 1;
            gateway.nextHeartbeatAt_ = time(0) + 100;
            gateway.poll();
            assert(gateway.joinPreparationStep_ == 0 && gateway.pendingJoinSeconds_ == 0);
        }
        Gateway unsupported("/dev/null", 115200, "/tmp", "", "", "");
        unsigned char reply[] = {0, 0x80, 0, 14, 2, 0, 0};
        unsupported.handleEzspFrame(std::vector<unsigned char>(reply, reply + sizeof(reply)));
        assert(!unsupported.online_ && unsupported.ashTransmitQueue_.empty());
        reply[3] = 13;
        unsupported.handleEzspFrame(std::vector<unsigned char>(reply, reply + 4));
        assert(!unsupported.online_ && unsupported.ezspVersion_ == 4);
    }
    static void networkJoinWindowDoesNotClearNetworkState() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        std::vector<unsigned char> status(6, 0);
        gateway.ezspVersion_ = 13;
        gateway.networkFormed_ = true;
        status[3] = 0x19;
        status[5] = 0x9c; /* NETWORK_OPENED */
        gateway.handleEzspFrame(status);
        assert(gateway.networkFormed_);
        status[5] = 0x9d; /* NETWORK_CLOSED */
        gateway.handleEzspFrame(status);
        assert(gateway.networkFormed_);
        status[5] = 0x91; /* NETWORK_DOWN */
        gateway.handleEzspFrame(status);
        assert(!gateway.networkFormed_);
    }
    static void reviewAndTransportRegressions() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        gateway.serialFd_ = open("/dev/null", O_WRONLY);
        assert(gateway.serialFd_ >= 0);
        gateway.sendEzspCommand(5, std::vector<unsigned char>());
        std::vector<unsigned char> original = gateway.ashPendingData_;
        gateway.sendEzspCommand(5, std::vector<unsigned char>());
        assert(gateway.ashTransmitQueue_.size() == 1);
        gateway.handleAshFrame(std::vector<unsigned char>(1, 0xa0));
        assert(gateway.ashRetries_ == 1 && gateway.ashPendingData_ == original);
        assert(gateway.ashSendSequence_ == 0);
        gateway.handleAshFrame(std::vector<unsigned char>(1, 0x81));
        gateway.pollAshTransmit();
        assert(gateway.ashSendSequence_ == 1 && gateway.ashTransmitQueue_.empty());
        gateway.ashAckDeadline_ = time(0) - 1;
        gateway.pollAshTransmit();
        assert(gateway.ashRetries_ == 1);
        gateway.handleAshFrame(std::vector<unsigned char>(1, 0x10)); /* gap */
        assert(gateway.ashReceiveSequence_ == 0);
        gateway.handleAshFrame(std::vector<unsigned char>(1, 0x00));
        assert(gateway.ashReceiveSequence_ == 1);
        gateway.handleAshFrame(std::vector<unsigned char>(1, 0x08)); /* duplicate */
        assert(gateway.ashReceiveSequence_ == 1);

        const unsigned short node = 0x4321;
        Gateway::InterviewState state;
        state.ieeeAddress = "0x00124b00000000ab";
        state.operations.push_back(Gateway::InterviewOperation());
        gateway.interviews_[node] = state;
        std::vector<unsigned char> response;
        assert(gateway.interviews_[node].operations[0].type == Gateway::InterviewActiveEndpoints);

        const Gateway::InterviewOperationType types[] = {
            Gateway::InterviewActiveEndpoints, Gateway::InterviewSimpleDescriptor,
            Gateway::InterviewBind, Gateway::InterviewBasicRead,
            Gateway::InterviewAttributeRead, Gateway::InterviewConfigureReporting};
        const unsigned short clusters[] = {0x8005, 0x8004, 0x8021, 0, 0x0402, 0x0402};
        gateway.nodeAddresses_[node] = state.ieeeAddress;
        for (unsigned int i = 0; i < 6; ++i) {
            state.operations[0] = Gateway::InterviewOperation();
            state.operations[0].type = types[i];
            state.operations[0].cluster = clusters[i];
            gateway.interviews_[node] = state;
        response.assign(32, 0);
            response[3] = static_cast<unsigned char>(clusters[i]);
            response[4] = static_cast<unsigned char>(clusters[i] >> 8);
            if (i >= 3) { response[1] = 0x04; response[2] = 0x01; }
            response[14] = 0x21; response[15] = 0x43;
            response[18] = 13;
            if (i == 1) response[23] = 8; /* valid empty simple descriptor */
            if (i >= 3) { response[19] = 0x18; response[21] = i == 5 ? 7 : 1; }
            gateway.handleIncomingMessage(response);
            assert(gateway.interviews_.count(node) == 1);
            assert(gateway.interviews_[node].operations[0].type == types[i]);
        }

        Gateway::InterviewOperation& operation = gateway.interviews_[node].operations[0];
        operation.attempts = 1;
        gateway.failInterviewOperation(node, "buffer full", true);
        assert(!gateway.interviews_[node].paused && operation.retryAt > time(0));
        operation.attempts = 3;
        gateway.failInterviewOperation(node, "buffer full", true);
        assert(gateway.interviews_[node].paused);

        /* An unrelated sendUnicast response must not reject this interview. */
        gateway.ezspVersion_ = 8;
        gateway.interviewRequestInFlight_ = true;
        gateway.interviewSendAccepted_ = false;
        gateway.interviewNode_ = node;
        gateway.interviewEzspSequence_ = 42;
        operation.attempts = 1;
        gateway.interviews_[node].paused = false;
        const unsigned char rejected[] = {41, 0x80, 1, 0x34, 0, 0x18};
        std::vector<unsigned char> reply(rejected, rejected + sizeof(rejected));
        gateway.handleEzspFrame(reply);
        assert(gateway.interviewRequestInFlight_);
        reply[0] = 42;
        gateway.handleEzspFrame(reply);
        assert(!gateway.interviewRequestInFlight_ && !gateway.interviews_[node].paused);

        /* Decode the outbound enrollment reply and check reversed endpoints. */
        gateway.ashPendingData_.clear();
        gateway.ashTransmitQueue_.clear();
        gateway.ashTransmitSensitiveQueue_.clear();
        gateway.devices_[state.ieeeAddress].zigbee.ensure().iasZoneId = 4;
        response.assign(26, 0);
        response[1] = 4; response[2] = 1;
        response[3] = 0; response[4] = 5;
        response[5] = 3; response[6] = 1;
        response[14] = 0x21; response[15] = 0x43;
        response[18] = 7;
        response[19] = 0x19; response[20] = 9; response[21] = 1;
        gateway.handleIncomingMessage(response);
        std::vector<unsigned char> command = gateway.ashPendingData_;
        unsigned char random = 0x42;
        for (unsigned long i = 0; i < command.size(); ++i) {
            command[i] ^= random;
            random = (random & 1) ? static_cast<unsigned char>((random >> 1) ^ 0xb8) :
                                   static_cast<unsigned char>(random >> 1);
        }
        assert(command.size() >= 14 && command[12] == 1 && command[13] == 3);
    }
    static bool parseSimpleDescriptor(const Gateway& gateway, const std::vector<unsigned char>& message,
                                      EndpointDescriptor* descriptor) {
        return gateway.parseSimpleDescriptor(message, descriptor);
    }
    static void buildConfiguration(Gateway* gateway, unsigned short node, const std::string& ieee,
                                   const EndpointDescriptor& endpoint) {
        Gateway::InterviewState state;
        state.ieeeAddress = ieee;
        gateway->interviews_[node] = state;
        gateway->devices_[ieee].ieeeAddress = ieee;
        gateway->devices_[ieee].zigbee.ensure().endpoints.push_back(endpoint);
        gateway->buildConfigurationOperations(node);
    }
    static unsigned long operationCount(const Gateway& gateway, unsigned short node) {
        return gateway.interviews_.find(node)->second.operations.size();
    }
    static int operationType(const Gateway& gateway, unsigned short node, unsigned long index) {
        return static_cast<int>(gateway.interviews_.find(node)->second.operations[index].type);
    }
    static unsigned short operationCluster(const Gateway& gateway, unsigned short node, unsigned long index) {
        return gateway.interviews_.find(node)->second.operations[index].cluster;
    }
    static void th01HerdsmanReportingPlan() {
        const std::string ieee = "0x00124b002a52140a";
        const unsigned short node = 0x1234;
        EndpointDescriptor endpoint;
        endpoint.endpoint = 1;
        endpoint.inputClusters.push_back(0x0402);
        endpoint.inputClusters.push_back(0x0405);
        endpoint.inputClusters.push_back(0x0001);
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        gateway.devices_[ieee].manufacturerName = "eWeLink";
        gateway.devices_[ieee].modelId = "TH01";
        buildConfiguration(&gateway, node, ieee, endpoint);
        const std::vector<Gateway::InterviewOperation>& operations = gateway.interviews_[node].operations;
        assert(operations.size() == 9);
        for (unsigned int i = 0; i < 3; ++i) assert(operations[i].type == Gateway::InterviewBind);
        assert(operations[0].cluster == 0x0402 && operations[1].cluster == 0x0405 && operations[2].cluster == 1);
        assert(operations[3].type == Gateway::InterviewConfigureReporting && operations[3].cluster == 0x0402);
        assert(operations[3].dataType == 0x29 && operations[3].minimumInterval == 30 &&
               operations[3].maximumInterval == 3600 && operations[3].reportableChange == 20);
        assert(operations[4].type == Gateway::InterviewConfigureReporting && operations[4].cluster == 0x0405);
        assert(operations[4].dataType == 0x21 && operations[4].minimumInterval == 30 &&
               operations[4].maximumInterval == 3600 && operations[4].reportableChange == 100);
        assert(operations[5].type == Gateway::InterviewConfigureReporting && operations[5].attribute == 0x0020);
        assert(operations[5].dataType == 0x20 && operations[5].minimumInterval == 3600 &&
               operations[5].maximumInterval == 65000 && operations[5].reportableChange == 0);
        assert(operations[6].type == Gateway::InterviewAttributeRead && operations[6].attribute == 0x0020);
        assert(operations[7].type == Gateway::InterviewConfigureReporting && operations[7].attribute == 0x0021);
        assert(operations[7].dataType == 0x20 && operations[7].minimumInterval == 3600 &&
               operations[7].maximumInterval == 65000 && operations[7].reportableChange == 0);
        assert(operations[8].type == Gateway::InterviewAttributeRead && operations[8].attribute == 0x0021);
        /* The TH01 quirk is scoped to eWeLink; other devices retain their plan. */
        gateway.devices_[ieee].manufacturerName = "Other";
        gateway.buildConfigurationOperations(node);
        assert(operations.size() == 10 && operations[3].maximumInterval == 300);
    }
    static bool pauseAndWake(Gateway* gateway, unsigned short node) {
        Gateway::InterviewState state;
        Gateway::InterviewOperation operation;
        state.ieeeAddress = "0x00124b0000000011";
        operation.attempts = 2;
        state.operations.push_back(operation);
        gateway->interviews_[node] = state;
        gateway->interviewRequestInFlight_ = true;
        gateway->interviewNode_ = node;
        gateway->failInterviewOperation(node, "test failure");
        if (!gateway->interviews_[node].paused) return false;
        gateway->wakeInterview(node);
        return !gateway->interviews_[node].paused;
    }
    static bool retryKeepsTransactionSequence(Gateway* gateway, unsigned short node) {
        Gateway::InterviewState state;
        Gateway::InterviewOperation operation;
        state.ieeeAddress = "0x00124b0000000012";
        state.operations.push_back(operation);
        gateway->interviews_[node] = state;
        gateway->sendInterviewOperation(node, &gateway->interviews_[node].operations[0]);
        unsigned char sequence = gateway->interviews_[node].operations[0].transactionSequence;
        gateway->failInterviewOperation(node, "test timeout");
        gateway->sendInterviewOperation(node, &gateway->interviews_[node].operations[0]);
        return gateway->interviews_[node].operations[0].transactionSequence == sequence;
    }
    static bool interviewRetryContributesDeadline(Gateway* gateway, unsigned short node) {
        Gateway::InterviewState state;
        Gateway::InterviewOperation operation;
        operation.retryAt = time(0) + 5;
        state.operations.push_back(operation);
        gateway->online_ = true;
        gateway->networkFormed_ = true;
        gateway->coordinatorInitializationStep_ = 0;
        gateway->interviews_[node] = state;
        return gateway->nextPollDeadline() == operation.retryAt;
    }
    static bool migrateRejoinedInterview(Gateway* gateway, unsigned short oldNode, unsigned short newNode) {
        Gateway::InterviewState state;
        Gateway::InterviewOperation operation;
        const unsigned char eui64[] = {0x13, 0x00, 0x00, 0x00, 0x00, 0x4b, 0x12, 0x00};
        state.ieeeAddress = "0x00124b0000000013";
        state.operations.push_back(operation);
        gateway->interviews_[oldNode] = state;
        gateway->nodeAddresses_[oldNode] = state.ieeeAddress;
        gateway->interviewRequestInFlight_ = true;
        gateway->interviewNode_ = newNode;
        gateway->interviews_[newNode] = Gateway::InterviewState();
        gateway->nodeAddresses_[newNode] = "0x00124b00000000ff";
        gateway->registerNode(newNode, eui64);
        return gateway->nodeAddresses_.find(oldNode) == gateway->nodeAddresses_.end() &&
               gateway->nodeAddresses_[newNode] == state.ieeeAddress &&
               gateway->interviews_.find(oldNode) == gateway->interviews_.end() &&
               gateway->interviews_.find(newNode) != gateway->interviews_.end() &&
               !gateway->interviewRequestInFlight_;
    }
    static bool contactCloseDuringInterview(Gateway* gateway) {
        const unsigned short contactNode = 0x1234;
        const std::string ieee = "0x00124b0000000014";
        std::vector<unsigned char> payload(19, 0);
        Gateway::InterviewState interview;
        Gateway::InterviewOperation operation;
        interview.ieeeAddress = "0x00124b0000000015";
        interview.operations.push_back(operation);
        gateway->interviews_[0x5678] = interview;
        gateway->interviewRequestInFlight_ = true;
        gateway->interviewNode_ = 0x5678;
        gateway->nodeAddresses_[contactNode] = ieee;
        gateway->devices_[ieee].ieeeAddress = ieee;
        gateway->devices_[ieee].contact.setPresent(true);
        gateway->devices_[ieee].contact.ensure().open = true;
        payload[1] = 0x04; payload[2] = 0x01; /* Home Automation profile */
        payload[3] = 0x00; payload[4] = 0x05; /* IAS Zone cluster */
        payload[12] = 99;
        payload[14] = 0x34; payload[15] = 0x12;
        payload[18] = 5;
        payload.push_back(0x19); payload.push_back(7); payload.push_back(0x00);
        payload.push_back(0x00); payload.push_back(0x00);
        gateway->handleIncomingMessage(payload);
        return gateway->devices_[ieee].contact.present() && !gateway->devices_[ieee].contact.read().open &&
               gateway->devices_[ieee].zigbee.read().linkQuality == 99;
    }
    static bool frameLogsContainDecodedDetails() {
        const unsigned short node = 0x1234;
        const std::string ieee = "0x00124b0000000020";
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        std::vector<unsigned char> request;
        std::vector<unsigned char> incoming(24, 0);
        gateway.logs_.clear();
        gateway.nodeAddresses_[node] = ieee;
        gateway.devices_[ieee].ieeeAddress = ieee;
        gateway.devices_[ieee].name = "Kitchen sensor";
        request.push_back(0x10); request.push_back(7); request.push_back(0x00); request.push_back(0x00); request.push_back(0x00);
        gateway.sendApsUnicast(node, 0x0104, 0x0402, 1, 1, request, 9);
        if (gateway.logsJson().find("FRAME TX recipient=Kitchen sensor ieee=0x00124b0000000020") == std::string::npos ||
            gateway.logsJson().find("Read Attributes") == std::string::npos ||
            gateway.logsJson().find("payload=1007000000") == std::string::npos) return false;
        incoming[1] = 0x04; incoming[2] = 0x01; incoming[3] = 0x02; incoming[4] = 0x04;
        incoming[5] = 1; incoming[6] = 1; incoming[11] = 44; incoming[12] = 88; incoming[13] = 0xd8;
        incoming[14] = 0x34; incoming[15] = 0x12; incoming[18] = 5;
        incoming[19] = 0x18; incoming[20] = 8; incoming[21] = 0x0a; incoming[22] = 0; incoming[23] = 0;
        gateway.handleIncomingMessage(incoming);
        return gateway.logsJson().find("FRAME RX sender=Kitchen sensor ieee=0x00124b0000000020") != std::string::npos &&
               gateway.logsJson().find("lqi=88 rssi=-40") != std::string::npos &&
               gateway.logsJson().find("Report Attributes") != std::string::npos;
    }
    static bool remoteKeyEventsAreLogged() {
        const unsigned short node = 0x4321;
        const std::string ieee = "0x00124b0000000021";
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        EndpointDescriptor endpoint;
        std::vector<unsigned char> move(1, 80);
        endpoint.endpoint = 1;
        endpoint.outputClusters.push_back(0x0006);
        endpoint.outputClusters.push_back(0x0008);
        gateway.nodeAddresses_[node] = ieee;
        gateway.devices_[ieee].ieeeAddress = ieee;
        gateway.devices_[ieee].zigbee.ensure().endpoints.push_back(endpoint);
        gateway.classifyDevice(&gateway.devices_[ieee]);
        gateway.logs_.clear();
        gateway.logRemoteKeyEvent(node, 0x0006, 1, 0, 0x01, std::vector<unsigned char>());
        gateway.logRemoteKeyEvent(node, 0x0008, 1, 0, 0x05, move);
        return gateway.logsJson().find("Remote key event") != std::string::npos &&
               gateway.logsJson().find("action=on") != std::string::npos &&
               gateway.logsJson().find("action=move") != std::string::npos;
    }
    static void tradfriFirmwareBindingAndOta() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        const std::string ieee = "0x70ac08fffe75827b";
        gateway.nodeAddresses_[0xd938] = ieee;
        Device& device = gateway.devices_[ieee];
        device.ieeeAddress = ieee;
        device.modelId = "TRADFRI on/off switch";
        gateway.coordinatorIeeeAddress_.assign(8, 0x11);
        const char* versions[] = {"2.2.010", "2.3.074", "2.3.075", "24.4.6"};
        for (unsigned int i = 0; i < 4; ++i) {
            device.softwareBuildId = versions[i];
            gateway.logs_.clear();
            gateway.sendZdoBindRequest(0xd938, 1, 6, ieee, 42, 1);
            const std::string expected = i < 2 ? "payload=2a7b8275feff08ac70010600018503" :
                "payload=2a7b8275feff08ac7001060003111111111111111101";
            assert(gateway.logsJson().find(expected) != std::string::npos);
        }
        std::vector<unsigned char> incoming(19, 0);
        incoming[1] = 4; incoming[2] = 1; incoming[3] = 0x19;
        incoming[5] = 1; incoming[6] = 1;
        incoming[14] = 0x38; incoming[15] = 0xd9;
        const unsigned char query[] = {1, 14, 1, 1, 0x7c, 0x11, 0xc5, 0x11, 0x31, 6, 1, 0x22, 1, 0};
        incoming[18] = sizeof(query);
        incoming.insert(incoming.end(), query, query + sizeof(query));
        gateway.logs_.clear();
        gateway.handleIncomingMessage(incoming);
        assert(gateway.logsJson().find("payload=190e0298") != std::string::npos);
        EndpointDescriptor endpoint;
        endpoint.endpoint = 1;
        endpoint.outputClusters.push_back(6);
        device.zigbee.ensure().endpoints.push_back(endpoint);
        device.remote.setPresent(true);
        incoming.resize(22);
        incoming[3] = 6; incoming[18] = 3;
        incoming[19] = 1; incoming[20] = 42; incoming[21] = 1;
        for (unsigned char type = 0; type <= 5; ++type) {
            incoming[0] = type;
            incoming[6] = type < 2 ? 1 : 255;
            gateway.logs_.clear();
            gateway.handleIncomingMessage(incoming);
            assert((gateway.logsJson().find("action=on") != std::string::npos) == (type == 0));
            assert((gateway.logsJson().find("Duplicate remote command suppressed") != std::string::npos) == (type != 0));
            assert((gateway.logsJson().find("payload=182a0b0100") != std::string::npos) == (type < 2));
        }
    }
    static bool nativeRemoteBindingFlow() {
        const std::string remoteIeee = "0x00124b0000000099";
        const std::string lightIeee = "0x00124b0000000042";
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        EndpointDescriptor remoteEndpoint, lightEndpoint;
        remoteEndpoint.endpoint = 1;
        remoteEndpoint.outputClusters.push_back(0x0006);
        lightEndpoint.endpoint = 1;
        lightEndpoint.inputClusters.push_back(0x0004);
        lightEndpoint.inputClusters.push_back(0x0006);
        Device& remote = gateway.devices_[remoteIeee];
        remote.ieeeAddress = remoteIeee; remote.zigbee.ensure().networkAddress = 0x1234;
        remote.remote.setPresent(true); remote.zigbee.ensure().endpoints.push_back(remoteEndpoint);
        Device& light = gateway.devices_[lightIeee];
        light.ieeeAddress = lightIeee; light.zigbee.ensure().networkAddress = 0x2345;
        light.light.setPresent(true); light.zigbee.ensure().endpoints.push_back(lightEndpoint);
        gateway.nodeAddresses_[0x1234] = remoteIeee;
        gateway.coordinatorIeeeAddress_.assign(8, 0x11);
        gateway.online_ = true; gateway.networkFormed_ = true;
        gateway.coordinatorInitializationStep_ = 0;
        SceneGroup group; group.id = 42; group.name = "Room"; group.members.push_back(lightIeee);
        gateway.sceneGroups_.push_back(group);
        NativeRemoteBinding binding; binding.ieeeAddress = remoteIeee; binding.groupId = 42;
        gateway.nativeBindings_.push_back(binding);
        gateway.queueNativeBindingChanges(std::vector<NativeRemoteBinding>(), std::vector<SceneGroup>());
        if (gateway.nativeBindOperations_.size() != 2 ||
            !gateway.nativeBindOperations_[0].unbind ||
            gateway.nativeBindOperations_[1].groupId != 42) return false;
        gateway.pollNativeBindings();
        if (gateway.nativeBindOperations_[0].deadline == 0 ||
            gateway.logsJson().find("cluster=0x22") == std::string::npos) return false;
        std::vector<unsigned char> reply(21, 0);
        reply[3] = 0x22; reply[4] = 0x80;
        reply[14] = 0x34; reply[15] = 0x12; reply[18] = 2;
        reply[19] = gateway.nativeBindOperations_[0].sequence;
        reply[20] = 0x88; /* no old coordinator entry */
        gateway.handleIncomingMessage(reply);
        if (gateway.nativeBindOperations_.size() != 1) return false;
        gateway.pollNativeBindings();
        if (gateway.nativeBindOperations_[0].deadline == 0 ||
            gateway.logsJson().find("payload=") == std::string::npos) return false;
        reply[3] = 0x21; reply[19] = gateway.nativeBindOperations_[0].sequence;
        reply[20] = 0x84; /* group unsupported: bind to lights */
        gateway.handleIncomingMessage(reply);
        if (gateway.nativeBindOperations_.size() != 1 ||
            gateway.nativeBindOperations_[0].destinationMode != 3 ||
            gateway.nativeBindOperations_[0].targetIeee != lightIeee) return false;
        gateway.pollNativeBindings();
        reply[19] = gateway.nativeBindOperations_[0].sequence;
        reply[20] = 0;
        gateway.handleIncomingMessage(reply);
        if (!gateway.nativeBindOperations_.empty() ||
            gateway.nativeBindingStates_[remoteIeee] != "active") return false;
        RemoteSceneBinding sceneRule; sceneRule.ieeeAddress = remoteIeee; sceneRule.action = "on";
        sceneRule.groupId = 42; sceneRule.sceneIds.push_back(1);
        gateway.sceneBindings_.push_back(sceneRule);
        gateway.dispatchRemoteSceneAction(remoteIeee, "on");
        return gateway.sceneRetries_.empty();
    }
    static bool nativeIkeaFirmwareModes() {
        const std::string remoteIeee = "0x70ac08fffe75827b";
        const std::string lightIeee = "0x84ba20fffea0e73c";
        const char* versions[] = {"2.3.074", "2.3.075"};
        for (unsigned int v = 0; v < 2; ++v) {
            Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
            Device& remote = gateway.devices_[remoteIeee];
            EndpointDescriptor endpoint; endpoint.endpoint = 1; endpoint.outputClusters.push_back(6);
            remote.ieeeAddress = remoteIeee; remote.modelId = "TRADFRI on/off switch";
            remote.softwareBuildId = versions[v]; remote.remote.setPresent(true);
            remote.zigbee.ensure().endpoints.push_back(endpoint);
            Device& light = gateway.devices_[lightIeee];
            endpoint.outputClusters.clear(); endpoint.inputClusters.push_back(4); endpoint.inputClusters.push_back(6);
            light.ieeeAddress = lightIeee; light.light.setPresent(true); light.zigbee.ensure().endpoints.push_back(endpoint);
            SceneGroup group; group.id = 42; group.members.push_back(lightIeee);
            gateway.sceneGroups_.push_back(group);
            NativeRemoteBinding binding; binding.ieeeAddress = remoteIeee; binding.groupId = 42;
            gateway.nativeBindings_.push_back(binding);
            gateway.queueNativeBindingChanges(std::vector<NativeRemoteBinding>(), std::vector<SceneGroup>());
            if (gateway.nativeBindOperations_.size() != 2) return false;
            if (gateway.nativeBindOperations_[0].destinationMode != (v == 0 ? 1 : 3)) return false;
            if (gateway.nativeBindOperations_[1].destinationMode != (v == 0 ? 1 : 3)) return false;
            if (v == 0 && gateway.nativeBindOperations_[1].groupId != 42) return false;
            if (v == 1 && gateway.nativeBindOperations_[1].targetIeee != lightIeee) return false;
        }
        return true;
    }
    static bool styrbarArrowEventsAreLogged() {
        const unsigned short node = 0x4321;
        const std::string ieee = "0x00124b0000000022";
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        EndpointDescriptor endpoint;
        std::vector<unsigned char> incoming(18, 0);
        endpoint.endpoint = 1;
        endpoint.outputClusters.push_back(0x0003);
        endpoint.outputClusters.push_back(0x0006);
        endpoint.outputClusters.push_back(0x0008);
        endpoint.outputClusters.push_back(0x0019);
        endpoint.outputClusters.push_back(0x1000);
        gateway.nodeAddresses_[node] = ieee;
        gateway.devices_[ieee].ieeeAddress = ieee;
        gateway.devices_[ieee].name = "Styrbar";
        gateway.devices_[ieee].zigbee.ensure().endpoints.push_back(endpoint);
        gateway.classifyDevice(&gateway.devices_[ieee]);
        incoming[1] = 0x04; incoming[2] = 0x01; /* Home Automation */
        incoming[3] = 0x05; incoming[4] = 0x00; /* Scenes */
        incoming[5] = 1; incoming[6] = 1;
        incoming[14] = static_cast<unsigned char>(node); incoming[15] = static_cast<unsigned char>(node >> 8);
        incoming.push_back(9);
        incoming.push_back(0x05); incoming.push_back(0x7c); incoming.push_back(0x11);
        incoming.push_back(42); incoming.push_back(0x07);
        incoming.push_back(0x01); incoming.push_back(0x01); incoming.push_back(13); incoming.push_back(0);
        gateway.handleIncomingMessage(incoming);
        incoming[22] = 43; /* transaction sequence */
        incoming[24] = 0x00; /* group 0x0100, right */
        gateway.handleIncomingMessage(incoming);
        incoming[18] = 8;
        incoming[22] = 44;
        incoming[23] = 0x08;
        incoming[24] = 0x01; incoming[25] = 13; incoming[26] = 0;
        gateway.handleIncomingMessage(incoming);
        incoming[18] = 7;
        incoming[22] = 45;
        incoming[23] = 0x09;
        incoming[24] = 0; incoming[25] = 0;
        gateway.handleIncomingMessage(incoming);
        return gateway.logsJson().find("action=arrow_left_click") != std::string::npos &&
               gateway.logsJson().find("action=arrow_right_click") != std::string::npos &&
               gateway.logsJson().find("action=arrow_left_hold") != std::string::npos &&
               gateway.logsJson().find("action=arrow_left_release") != std::string::npos &&
               gateway.logsJson().find("payload=182a0b0700") != std::string::npos &&
               gateway.logsJson().find("Configure reporting response from 0x00124b0000000022: cluster 0x5") == std::string::npos;
    }
    static bool remoteBindingsAndIgnoredEvents() {
        const unsigned short node = 0x1234;
        const std::string remoteIeee = "remote";
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        Device& remote = gateway.devices_[remoteIeee];
        EndpointDescriptor remoteEndpoint; remoteEndpoint.endpoint = 1; remoteEndpoint.outputClusters.push_back(0x0006);
        remote.ieeeAddress = remoteIeee; remote.remote.setPresent(true); remote.zigbee.ensure().endpoints.push_back(remoteEndpoint);
        gateway.nodeAddresses_[node] = remoteIeee;
        Device& light = gateway.devices_["light"];
        EndpointDescriptor lightEndpoint; lightEndpoint.endpoint = 1; lightEndpoint.inputClusters.push_back(0x0004);
        lightEndpoint.inputClusters.push_back(0x0005); lightEndpoint.inputClusters.push_back(0x0006);
        light.ieeeAddress = "light"; light.zigbee.ensure().networkAddress = 0x2222; light.light.setPresent(true); light.zigbee.ensure().endpoints.push_back(lightEndpoint);
        SceneGroup group; group.id = 10; group.name = "Room"; group.members.push_back("light");
        Scene scene; scene.id = 1; scene.name = "Bright";
        SceneLightState state; state.ieeeAddress = "light"; state.on = true; state.brightnessPercent = -1;
        scene.states.push_back(state); group.scenes.push_back(scene); gateway.sceneGroups_.push_back(group);
        RemoteSceneBinding binding; binding.ieeeAddress = remoteIeee; binding.action = "arrow_left";
        binding.groupId = 10; binding.sceneIds.push_back(1); gateway.sceneBindings_.push_back(binding);
        gateway.online_ = true; gateway.networkFormed_ = true; gateway.coordinatorInitializationStep_ = 0;
        std::vector<unsigned char> leftClick; leftClick.push_back(0x01); leftClick.push_back(0x01); leftClick.push_back(13); leftClick.push_back(0);
        if (!gateway.logRemoteKeyEvent(node, 0x0005, 1, 0x117c, 0x07, leftClick) ||
            gateway.sceneRetries_.find(10) == gateway.sceneRetries_.end() || gateway.sceneRetries_[10].sceneId != 1) return false;
        if (!gateway.logRemoteKeyEvent(node, 0x0005, 1, 0x117c, 0x07, leftClick) ||
            gateway.sceneRetries_[10].retryRequested || gateway.selectedScenes_.count(10) != 0) return false;
        std::vector<unsigned char> unknownGroup; unknownGroup.push_back(0x02); unknownGroup.push_back(0); unknownGroup.push_back(13); unknownGroup.push_back(0);
        if (gateway.logRemoteKeyEvent(node, 0x0005, 1, 0x117c, 0x07, unknownGroup)) return false;
        std::vector<unsigned char> hold; hold.push_back(1); hold.push_back(0); hold.push_back(0);
        if (!gateway.logRemoteKeyEvent(node, 0x0005, 1, 0x117c, 0x08, hold) ||
            !gateway.logRemoteKeyEvent(node, 0x0005, 1, 0x117c, 0x09, std::vector<unsigned char>()) ||
            gateway.logRemoteKeyEvent(node, 0x0005, 1, 0x117c, 0x09, std::vector<unsigned char>())) return false;
        if (gateway.logRemoteKeyEvent(0x9999, 0x0006, 1, 0, 0x01, std::vector<unsigned char>())) return false;
        return gateway.sceneRetries_[10].sceneId == 1 && !gateway.sceneRetries_[10].retryRequested &&
               gateway.selectedScenes_.count(10) == 0;
    }
    static bool rejectsDuplicateSceneStates() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        std::string error;
        gateway.devices_["0xlight1"].ieeeAddress = "0xlight1";
        gateway.devices_["0xlight1"].light.setPresent(true);
        gateway.devices_["0xlight2"].ieeeAddress = "0xlight2";
        gateway.devices_["0xlight2"].light.setPresent(true);
        const char* configuration =
            "G\t1\tTest\nM\t1\t0xlight1\nM\t1\t0xlight2\nS\t1\t1\tScene\n"
            "T\t1\t1\t0xlight1\t1\t50\nT\t1\t1\t0xlight1\t0\t-1\n";
        return !gateway.setSceneConfiguration(configuration, &error) && error == "invalid scene configuration";
    }
    static bool rejectsInvalidSceneCrossReferences() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        Device light; light.ieeeAddress = "light1"; light.light.setPresent(true);
        Device otherLight; otherLight.ieeeAddress = "light2"; otherLight.light.setPresent(true);
        Device remote; remote.ieeeAddress = "remote1"; remote.remote.setPresent(true);
        gateway.devices_[light.ieeeAddress] = light;
        gateway.devices_[otherLight.ieeeAddress] = otherLight;
        gateway.devices_[remote.ieeeAddress] = remote;
        SceneGroup oldGroup; oldGroup.id = 77; oldGroup.name = "old";
        gateway.sceneGroups_.push_back(oldGroup);
        const char* invalid[] = {
            "G\t1\tRoom\nM\t1\tlight1\nS\t1\t1\tScene\nT\t1\t1\tlight1\t1\t50\nB\tremote1\ton\t2\t0\t1\n",
            "G\t1\tRoom\nM\t1\tlight1\nS\t1\t1\tScene\nT\t1\t1\tlight1\t1\t50\nB\tremote1\ton\t1\t0\t2\n",
            "G\t1\tRoom\nM\t1\tlight1\nS\t1\t1\tScene\nT\t1\t1\tlight1\t1\t50\nB\tmissing\ton\t1\t0\t1\n",
            "G\t1\tRoom\nM\t1\tlight1\nS\t1\t1\tScene\n",
            "G\t1\tRoom\nM\t1\tlight1\nS\t1\t1\tScene\nT\t1\t1\tlight2\t1\t50\n",
            "G\t1\tRoom\tTabbed\n",
            "G\t1\tRoom\nM\t1\tlight1\nS\t1\t1\tScene\tTabbed\nT\t1\t1\tlight1\t1\t50\n"
        };
        for (unsigned long i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            std::string error;
            if (gateway.setSceneConfiguration(invalid[i], &error) || error.empty() ||
                gateway.sceneGroups_.size() != 1 || gateway.sceneGroups_[0].id != 77 ||
                gateway.sceneGroups_[0].name != "old") return false;
        }
        return true;
    }
    static bool saveTriggeredProvisioning() {
        std::ostringstream path;
        path << "/tmp/microhad-zigbee-save-provision-" << static_cast<long>(getpid());
        std::string error;
        if (!ensureDirectory(path.str(), &error)) return false;
        const std::string lightIeee = "0x00124b0000000042";
        const std::string remoteIeee = "0x00124b0000000099";
        const std::string encoded =
            "G\t42\tKitchen\nM\t42\t0x00124b0000000042\nS\t42\t7\tDinner\n"
            "T\t42\t7\t0x00124b0000000042\t1\t50\nN\t0x00124b0000000099\t42\n";
        {
            Gateway gateway("/dev/null", 115200, path.str(), "", "", "");
            Device& light = gateway.devices_[lightIeee];
            EndpointDescriptor lightEndpoint;
            light.ieeeAddress = lightIeee; light.light.setPresent(true); light.zigbee.ensure().networkAddress = 0x2345;
            lightEndpoint.endpoint = 1;
            lightEndpoint.inputClusters.push_back(4); lightEndpoint.inputClusters.push_back(5);
            lightEndpoint.inputClusters.push_back(6); lightEndpoint.inputClusters.push_back(8);
            light.zigbee.ensure().endpoints.push_back(lightEndpoint);
            Device& remote = gateway.devices_[remoteIeee];
            EndpointDescriptor remoteEndpoint;
            remote.ieeeAddress = remoteIeee; remote.remote.setPresent(true); remote.zigbee.ensure().networkAddress = 0x1234;
            remoteEndpoint.endpoint = 1; remoteEndpoint.outputClusters.push_back(6);
            remote.zigbee.ensure().endpoints.push_back(remoteEndpoint);
            gateway.online_ = true; gateway.networkFormed_ = true;
            if (!gateway.setSceneConfiguration(encoded, &error)) return false;
            if (gateway.sceneProvisioning_[lightIeee].commands.size() != 2 ||
                gateway.sceneProvisioning_[lightIeee].deadline == 0 ||
                gateway.nativeBindOperations_.empty()) return false;
            const unsigned char transaction = gateway.sceneProvisioning_[lightIeee].commands[0].transaction;
            std::vector<unsigned char> response;
            response.push_back(0x09); response.push_back(transaction); response.push_back(0);
            response.push_back(0); response.push_back(42); response.push_back(0);
            if (gateway.handleSceneProvisionResponse(lightIeee, 4, transaction + 1, 0, response, 3) ||
                gateway.sceneProvisioning_[lightIeee].index != 0) return false;
            if (!gateway.handleSceneProvisionResponse(lightIeee, 4, transaction, 0, response, 3) ||
                gateway.sceneProvisioning_[lightIeee].index != 1) return false;
            gateway.pollSceneProvisioning();
            Gateway::SceneProvisionState& pending = gateway.sceneProvisioning_[lightIeee];
            if (pending.deadline == 0 || pending.commands[1].transaction == 0) return false;
            pending.deadline = time(0) - 1;
            gateway.pollSceneProvisioning();
            if (!pending.waitingForTraffic || pending.deadline != 0) return false;
            gateway.wakeSceneProvisioning(lightIeee);
            gateway.pollSceneProvisioning();
            if (pending.waitingForTraffic || pending.deadline == 0 || pending.index != 1) return false;
            response[1] = pending.commands[1].transaction;
            response.push_back(7);
            if (!gateway.handleSceneProvisionResponse(lightIeee, 5, response[1], 0, response, 3) ||
                !gateway.sceneProvisioning_.empty()) return false;
            if (!gateway.setSceneConfiguration(encoded, &error) || gateway.nativeBindOperations_.empty() ||
                gateway.sceneProvisioning_.empty()) return false;
        }
        {
            Gateway restarted("/dev/null", 115200, path.str(), "", "", "");
            Device& light = restarted.devices_[lightIeee];
            EndpointDescriptor endpoint;
            light.ieeeAddress = lightIeee; light.zigbee.ensure().networkAddress = 0x2345; light.light.setPresent(true);
            endpoint.endpoint = 1;
            endpoint.inputClusters.push_back(4); endpoint.inputClusters.push_back(5);
            endpoint.inputClusters.push_back(6); endpoint.inputClusters.push_back(8);
            light.zigbee.ensure().endpoints.push_back(endpoint);
            restarted.loadScenes();
            if (!restarted.sceneProvisioning_.empty() || !restarted.nativeBindOperations_.empty() ||
                restarted.nativeBindingStates_[remoteIeee] != "configured; not verified since restart") return false;
            restarted.ezspVersion_ = 13;
            std::vector<unsigned char> initialized(6, 0);
            initialized[3] = 0x17;
            restarted.networkFormed_ = true;
            restarted.handleEzspFrame(initialized);
            initialized[3] = 0x19; initialized[5] = 0x90;
            restarted.handleEzspFrame(initialized);
            if (!restarted.sceneProvisioning_.empty() || !restarted.nativeBindOperations_.empty() ||
                restarted.logsJson().find("FRAME TX recipient=") != std::string::npos) return false;
        }
        unlink((path.str() + "/scenes.conf").c_str());
        return rmdir(path.str().c_str()) == 0;
    }
    static bool sceneBusyDoesNotAdvanceSelection() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        Gateway::SceneRetry& retry = gateway.sceneRetries_[1];
        std::vector<unsigned char> reply(6, 0);
        gateway.ezspVersion_ = 8;
        retry.inFlight = true; retry.ezspSequence = 42; retry.sceneId = 2; retry.sentSceneId = 2;
        reply[0] = 42; reply[3] = 0x38; reply[4] = 0; reply[5] = 0xa1; /* EMBER_NETWORK_BUSY */
        gateway.handleEzspFrame(reply);
        long now = time(0);
        if (retry.inFlight || retry.attempts != 1 || retry.retryAt < now || retry.retryAt > now + 1 || !gateway.selectedScenes_.empty()) return false;
        for (int i = 2; i <= 5; ++i) {
            now = time(0);
            if (!gateway.scheduleSceneRetry(&retry, now) || retry.attempts != i || retry.retryAt != now + 1) return false;
        }
        now = time(0);
        if (!gateway.scheduleSceneRetry(&retry, now) || retry.attempts != 6 || retry.retryAt != now + 10) return false;
        return !gateway.scheduleSceneRetry(&retry, time(0)) && retry.retryAt == LONG_MAX;
    }
    static bool sceneRuntimePersistence() {
        const char* filename = "/tmp/microhad-zigbee-scenes-runtime";
        std::ifstream previousInput(filename);
        bool hadPrevious = previousInput.good();
        std::string previous((std::istreambuf_iterator<char>(previousInput)), std::istreambuf_iterator<char>());
        bool result = false;
        {
            std::ofstream output(filename, std::ios::trunc);
            if (output) { output << "S 12 4\nF 12 1\n"; output.close(); }
            if (output) {
                Gateway loaded("/dev/null", 115200, "/tmp", "", "", "");
                loaded.loadSceneRuntime();
                bool loadedOk = loaded.selectedScenes_.count(12) == 1 && loaded.selectedScenes_[12] == 4 &&
                                loaded.sceneGroupOff_.count(12) == 1 && loaded.sceneGroupOff_[12];
                loaded.selectedScenes_[13] = 9;
                loaded.sceneGroupOff_[13] = false;
                loaded.saveSceneRuntime();
                Gateway restarted("/dev/null", 115200, "/tmp", "", "", "");
                restarted.loadSceneRuntime();
                result = loadedOk && restarted.selectedScenes_.count(13) == 1 && restarted.selectedScenes_[13] == 9 &&
                         restarted.sceneGroupOff_.count(13) == 1 && !restarted.sceneGroupOff_[13];
            }
        }
        if (hadPrevious) {
            std::ofstream restore(filename, std::ios::trunc);
            if (restore) { restore << previous; restore.close(); }
            if (!restore) result = false;
        } else if (unlink(filename) != 0) result = false;
        return result;
    }
    static bool nativeAndFallbackSceneOutcomes() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        gateway.online_ = true; gateway.networkFormed_ = true;
        gateway.coordinatorInitializationStep_ = 0; gateway.ezspVersion_ = 8;
        EndpointDescriptor nativeEndpoint; nativeEndpoint.endpoint = 1;
        nativeEndpoint.inputClusters.push_back(0x0004); nativeEndpoint.inputClusters.push_back(0x0005);
        nativeEndpoint.inputClusters.push_back(0x0006);
        EndpointDescriptor fallbackEndpoint; fallbackEndpoint.endpoint = 1; fallbackEndpoint.inputClusters.push_back(0x0006);
        Device& native = gateway.devices_["native"];
        native.ieeeAddress = "native"; native.zigbee.ensure().networkAddress = 0x1001; native.light.setPresent(true);
        native.zigbee.ensure().endpoints.push_back(nativeEndpoint);
        Device& fallback = gateway.devices_["fallback"];
        fallback.ieeeAddress = "fallback"; fallback.zigbee.ensure().networkAddress = 0x1002; fallback.light.setPresent(true);
        fallback.zigbee.ensure().endpoints.push_back(fallbackEndpoint);
        SceneGroup mixed; mixed.id = 3; mixed.name = "Mixed";
        mixed.members.push_back("native"); mixed.members.push_back("fallback");
        Scene scene; scene.id = 1; scene.name = "On";
        SceneLightState nativeState; nativeState.ieeeAddress = "native"; nativeState.on = true; nativeState.brightnessPercent = -1;
        SceneLightState fallbackState; fallbackState.ieeeAddress = "fallback"; fallbackState.on = true; fallbackState.brightnessPercent = -1;
        scene.states.push_back(nativeState); scene.states.push_back(fallbackState); mixed.scenes.push_back(scene);
        gateway.sceneGroups_.push_back(mixed);
        std::string error;
        if (!gateway.activateScene(3, 1, &error) || !gateway.sceneRetries_[3].inFlight ||
            gateway.logsJson().find("profile=0x104 cluster=0x5") == std::string::npos ||
            gateway.devices_["fallback"].light.read().on || gateway.selectedScenes_.count(3) != 0) return false;
        std::vector<unsigned char> reply(6, 0);
        reply[0] = gateway.sceneRetries_[3].ezspSequence; reply[3] = 0x38; reply[5] = 0;
        gateway.handleEzspFrame(reply);
        if (gateway.selectedScenes_[3] != 1 || !gateway.devices_["fallback"].light.read().on ||
            gateway.devices_["native"].light.read().on || gateway.logsJson().find("Scene recalled: Mixed / On") == std::string::npos) return false;
        SceneGroup fallbackOnly; fallbackOnly.id = 4; fallbackOnly.name = "Fallback";
        fallbackOnly.members.push_back("fallback");
        Scene fallbackScene; fallbackScene.id = 2; fallbackScene.name = "Off";
        SceneLightState offState; offState.ieeeAddress = "fallback"; offState.on = false; offState.brightnessPercent = -1;
        fallbackScene.states.push_back(offState); fallbackOnly.scenes.push_back(fallbackScene);
        gateway.sceneGroups_.push_back(fallbackOnly);
        if (!gateway.activateScene(4, 2, &error) || gateway.sceneRetries_[4].retryAt != LONG_MAX ||
            gateway.selectedScenes_[4] != 2 || gateway.devices_["fallback"].light.read().on ||
            gateway.logsJson().find("profile=0x104 cluster=0x6") == std::string::npos) return false;
        return true;
    }
    static void sceneBusyUnicastFallback() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        gateway.ezspVersion_ = 8;
        gateway.online_ = true; gateway.networkFormed_ = true;
        gateway.coordinatorInitializationStep_ = 0;
        SceneGroup group; group.id = 1; group.name = "Test";
        Scene scene; scene.id = 2; scene.name = "Scene";
        for (int i = 0; i < 2; ++i) {
            std::string ieee = i == 0 ? "0xlight1" : "0xlight2";
            Device& device = gateway.devices_[ieee];
            device.ieeeAddress = ieee; device.zigbee.ensure().networkAddress = 0x1234 + i;
            EndpointDescriptor endpoint; endpoint.endpoint = 3; endpoint.profileId = 0x0104;
            endpoint.inputClusters.push_back(4); endpoint.inputClusters.push_back(5);
            endpoint.inputClusters.push_back(6);
            device.zigbee.ensure().endpoints.push_back(endpoint);
            group.members.push_back(ieee);
            SceneLightState state; state.ieeeAddress = ieee; state.on = true; state.brightnessPercent = -1;
            scene.states.push_back(state);
        }
        group.scenes.push_back(scene); gateway.sceneGroups_.push_back(group);
        std::string error;
        assert(gateway.activateScene(1, 2, &error));
        Gateway::SceneRetry& retry = gateway.sceneRetries_[1];
        std::vector<unsigned char> reply(6, 0);
        reply[0] = retry.ezspSequence; reply[3] = 0x38; reply[5] = 0xa1;
        gateway.handleEzspFrame(reply);
        assert(retry.unicast && !retry.inFlight && gateway.selectedScenes_.empty());
        gateway.pollSceneRetries();
        assert(retry.inFlight && retry.memberIndex == 0);
        assert(gateway.logsJson().find("dst_ep=3") != std::string::npos);
        reply[0] = retry.ezspSequence; reply[3] = 0x34; reply[5] = 0xa1;
        gateway.handleEzspFrame(reply);
        assert(!retry.inFlight && retry.memberIndex == 0 && gateway.selectedScenes_.empty());
        retry.retryAt = time(0);
        gateway.pollSceneRetries();
        for (int i = 0; i < 2; ++i) {
            reply[0] = retry.ezspSequence; reply[5] = 0;
            gateway.handleEzspFrame(reply);
            assert(gateway.selectedScenes_.empty());
            gateway.pollSceneRetries();
        }
        assert(gateway.selectedScenes_[1] == 2 && retry.retryAt == LONG_MAX);
        assert(gateway.turnOffSceneGroup(1, &error));
        assert(!retry.unicast);
        reply[0] = retry.ezspSequence; reply[3] = 0x38; reply[5] = 0xa1;
        gateway.handleEzspFrame(reply); gateway.pollSceneRetries();
        assert(retry.unicast && retry.inFlight);
        /* A new selection supersedes an outstanding fallback, even on timeout. */
        assert(gateway.activateScene(1, 2, &error));
        retry.sentAt = time(0) - 3;
        gateway.pollSceneRetries();
        assert(retry.unicast && retry.memberIndex == 0 && retry.inFlight && !retry.sentOff);
    }
    static void sceneDeliveryFixture(Gateway& gateway) {
        gateway.ezspVersion_ = 8;
        gateway.online_ = true; gateway.networkFormed_ = true;
        gateway.coordinatorInitializationStep_ = 0;
        SceneGroup group; group.id = 1; group.name = "Room";
        for (int i = 0; i < 3; ++i) {
            std::ostringstream name; name << "light" << i;
            Device& light = gateway.devices_[name.str()];
            light.ieeeAddress = name.str(); light.zigbee.ensure().networkAddress = 0x1234 + i;
            EndpointDescriptor endpoint; endpoint.endpoint = 1;
            endpoint.inputClusters.push_back(4); endpoint.inputClusters.push_back(5);
            endpoint.inputClusters.push_back(6); light.zigbee.ensure().endpoints.push_back(endpoint);
            group.members.push_back(name.str());
        }
        for (int i = 1; i <= 3; ++i) {
            Scene scene; scene.id = i; scene.name = "Scene";
            for (unsigned long j = 0; j < group.members.size(); ++j) {
                SceneLightState state; state.ieeeAddress = group.members[j];
                state.on = true; state.brightnessPercent = -1; scene.states.push_back(state);
            }
            group.scenes.push_back(scene);
        }
        gateway.sceneGroups_.push_back(group);
        Device& remote = gateway.devices_["remote"];
        remote.ieeeAddress = "remote"; remote.remote.setPresent(true);
        EndpointDescriptor endpoint; endpoint.endpoint = 1; endpoint.outputClusters.push_back(6);
        remote.zigbee.ensure().endpoints.push_back(endpoint); gateway.nodeAddresses_[0x4321] = "remote";
        RemoteSceneBinding binding; binding.ieeeAddress = "remote"; binding.action = "on";
        binding.groupId = 1;
        binding.sceneIds.push_back(1); binding.sceneIds.push_back(2); binding.sceneIds.push_back(3);
        gateway.sceneBindings_.push_back(binding);
        binding.action = "off"; binding.off = true; gateway.sceneBindings_.push_back(binding);
    }
    static bool pendingSceneKeepsLatestRemoteAction() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        sceneDeliveryFixture(gateway);
        const std::vector<unsigned char> empty;
        for (int transaction = 1; transaction <= 5; ++transaction) {
            if (!gateway.logRemoteKeyEvent(0x4321, 6, 1, 0, 1, empty, transaction)) return false;
            if (!gateway.logRemoteKeyEvent(0x4321, 6, 1, 0, 1, empty, transaction)) return false;
        }
        Gateway::SceneRetry& retry = gateway.sceneRetries_[1];
        if (retry.sceneId != 2 || retry.sentSceneId != 1 || retry.retryRequested) return false;
        std::vector<unsigned char> reply(6, 0);
        reply[0] = retry.ezspSequence; reply[3] = 0x38;
        gateway.handleEzspFrame(reply); // Old success must not commit scene 1.
        if (!gateway.selectedScenes_.empty() || retry.inFlight) return false;
        gateway.pollSceneRetries();
        if (retry.sentSceneId != 2 || !retry.inFlight) return false;
        reply[0] = retry.ezspSequence; gateway.handleEzspFrame(reply);
        if (gateway.selectedScenes_[1] != 2) return false;
        gateway.logRemoteKeyEvent(0x4321, 6, 1, 0, 0, empty, 6);
        gateway.logRemoteKeyEvent(0x4321, 6, 1, 0, 1, empty, 7);
        if (retry.off || retry.sceneId != 2) return false; // Restore pending off's current scene.
        gateway.logRemoteKeyEvent(0x4321, 6, 1, 0, 1, empty, 8);
        if (retry.sceneId != 3) return false;
        reply[0] = retry.ezspSequence; reply[5] = 0xa1;
        gateway.handleEzspFrame(reply); // Old off failure must not commit off.
        return !retry.inFlight && !retry.off && !gateway.sceneGroupOff_[1] && gateway.selectedScenes_[1] == 2;
    }
    static void remoteCommandDeduplication() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        sceneDeliveryFixture(gateway);
        std::vector<unsigned char> empty;
        assert(gateway.logRemoteKeyEvent(0x4321, 6, 1, 0, 1, empty, 255));
        int64_t first = gateway.remoteCommands_["remote"].front().receivedAt;
        assert(gateway.logRemoteKeyEvent(0x4321, 6, 1, 0, 1, empty, 255));
        assert(gateway.remoteCommands_["remote"].size() == 1 &&
               gateway.remoteCommands_["remote"].front().receivedAt == first);
        assert(gateway.logRemoteKeyEvent(0x4321, 6, 1, 0, 1, empty, 0));
        assert(gateway.sceneRetries_[1].sceneId == 2); // Transaction wrap is a new press.
        gateway.remoteCommands_["remote"].front().receivedAt -= 2001;
        assert(gateway.logRemoteKeyEvent(0x4321, 6, 1, 0, 1, empty, 255));
        assert(gateway.sceneRetries_[1].sceneId == 3);
        gateway.devices_["other"] = gateway.devices_["remote"];
        gateway.devices_["other"].ieeeAddress = "other"; gateway.nodeAddresses_[0x4322] = "other";
        assert(gateway.logRemoteKeyEvent(0x4322, 6, 1, 0, 1, empty, 255));
        assert(gateway.remoteCommands_["other"].size() == 1);
        std::vector<unsigned char> hold(3, 0); hold[0] = 1;
        assert(gateway.logRemoteKeyEvent(0x4321, 5, 1, 0x117c, 8, hold, 1));
        assert(gateway.logRemoteKeyEvent(0x4321, 5, 1, 0x117c, 9, empty, 2));
        assert(gateway.remoteSceneHolds_.empty());
        assert(gateway.logRemoteKeyEvent(0x4321, 5, 1, 0x117c, 8, hold, 1));
        assert(gateway.remoteSceneHolds_.empty()); // Duplicate hold must not resurrect released state.
        assert(gateway.logRemoteKeyEvent(0x4321, 5, 1, 0x117c, 9, empty, 2));
        for (int i = 10; i < 35; ++i) gateway.logRemoteKeyEvent(0x4321, 6, 1, 0, 1, empty, i);
        assert(gateway.remoteCommands_["remote"].size() == 16);
        Gateway protocol("/dev/null", 115200, "/tmp", "", "", "");
        sceneDeliveryFixture(protocol);
        std::vector<unsigned char> packet;
        packet.push_back(1); packet.push_back(240); packet.push_back(1);
        for (int i = 0; i < 2; ++i)
            protocol.handleApsMessage(0x4321, 0x104, 6, 1, 1, 0, 0, 0, 0, 200, -70, packet);
        unsigned int responses = 0, events = 0;
        for (unsigned long i = 0; i < protocol.logs_.size(); ++i) {
            if (protocol.logs_[i].find("payload=18f00b0100") != std::string::npos) ++responses;
            if (protocol.logs_[i].find("Remote key event:") != std::string::npos) ++events;
        }
        assert(responses == 2 && events == 1 && protocol.sceneRetries_[1].sceneId == 1);
    }
#if MICROHAD_ZIGGURAT_ENABLED
    static void zigguratSceneUnicastFallback() {
        Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
        sceneDeliveryFixture(gateway);
        std::string error;
        assert(gateway.activateScene(1, 1, &error));
        Gateway::SceneRetry& retry = gateway.sceneRetries_[1];
        gateway.zigguratSendResult(retry.ezspSequence, false, 1);
        assert(retry.unicast && !retry.inFlight && retry.memberIndex == 0);
        gateway.pollSceneRetries();
        assert(retry.inFlight && retry.unicast && gateway.selectedScenes_.empty());
        gateway.zigguratSendResult(retry.ezspSequence, true, 0);
        gateway.pollSceneRetries();
        assert(retry.memberIndex == 1 && gateway.selectedScenes_.empty());
        assert(gateway.activateScene(1, 2, &error));
        gateway.zigguratSendResult(retry.ezspSequence, true, 0);
        gateway.pollSceneRetries();
        assert(retry.memberIndex == 0 && retry.unicast && retry.sentSceneId == 2);
        gateway.zigguratSendResult(retry.ezspSequence, true, 1);
        assert(!retry.inFlight && retry.memberIndex == 0 && gateway.selectedScenes_.empty());
        retry.retryAt = time(0); gateway.pollSceneRetries();
        for (int i = 0; i < 3; ++i) {
            assert(retry.inFlight && retry.memberIndex == static_cast<unsigned long>(i));
            assert(gateway.selectedScenes_.empty());
            gateway.zigguratSendResult(retry.ezspSequence, true, 0); gateway.pollSceneRetries();
        }
        assert(gateway.selectedScenes_[1] == 2 && retry.retryAt == LONG_MAX);
        assert(gateway.activateScene(1, 3, &error));
        retry.sentAt = time(0) - 3; gateway.pollSceneRetries();
        assert(retry.inFlight && retry.unicast && retry.memberIndex == 0);
        for (int i = 0; i < 7; ++i) {
            gateway.zigguratSendResult(retry.ezspSequence, true, 1);
            if (i < 6) { retry.retryAt = time(0); gateway.pollSceneRetries(); }
        }
        assert(retry.retryAt == LONG_MAX && !retry.inFlight && gateway.selectedScenes_[1] == 2);
        assert(gateway.turnOffSceneGroup(1, &error));
        assert(!retry.unicast);
        gateway.zigguratSendResult(retry.ezspSequence, false, 1); gateway.pollSceneRetries();
        assert(retry.unicast && retry.sentOff);
        for (int i = 0; i < 3; ++i) {
            gateway.zigguratSendResult(retry.ezspSequence, true, 0); gateway.pollSceneRetries();
        }
        assert(gateway.sceneGroupOff_[1] && retry.retryAt == LONG_MAX);
    }
#endif

};

static void testAshRoundTrip() {
    std::vector<unsigned char> payload;
    std::vector<unsigned char> encoded;
    std::vector<unsigned char> frame;
    AshTransport transport;
    payload.push_back(0x7e);
    payload.push_back(0x7d);
    payload.push_back(0x11);
    payload.push_back(0x13);
    payload.push_back(0x18);
    payload.push_back(0x1a);
    payload.push_back(0x12);
    encoded = AshTransport::encodeData(0x00, payload);
    assert(encoded.size() >= 16);
    assert(encoded[0] == 0 && encoded[1] == 0x7d && encoded[2] == 0x5e &&
           encoded[3] == 0x7d && encoded[4] == 0x5d && encoded[5] == 0x7d && encoded[6] == 0x31 &&
           encoded[7] == 0x7d && encoded[8] == 0x33 && encoded[9] == 0x7d && encoded[10] == 0x38 &&
           encoded[11] == 0x7d && encoded[12] == 0x3a && encoded[13] == 0x12 && encoded[encoded.size() - 1] == 0x7e);
    transport.feed(&encoded[0], encoded.size());
    assert(transport.popFrame(&frame));
    assert(frame.size() == 8);
    assert(frame[0] == 0x00 && frame[1] == 0x7e && frame[2] == 0x7d && frame[3] == 0x11 &&
           frame[4] == 0x13 && frame[5] == 0x18 && frame[6] == 0x1a && frame[7] == 0x12);
    /* A CANCEL after a complete frame must only discard a partial frame. */
    encoded.push_back(0x1a);
    transport.feed(&encoded[0], encoded.size());
    assert(transport.popFrame(&frame));
}

static void testAshRejectsBadCrc() {
    std::vector<unsigned char> payload;
    std::vector<unsigned char> encoded;
    std::vector<unsigned char> frame;
    AshTransport transport;
    payload.push_back(0x44);
    encoded = AshTransport::encodeData(0x00, payload);
    encoded[1] ^= 0x01;
    transport.feed(&encoded[0], encoded.size());
    assert(!transport.popFrame(&frame));
}

static void testAshFragmentationAndFrameOrdering() {
    AshTransport transport;
    std::vector<unsigned char> empty;
    std::vector<unsigned char> firstPayload;
    std::vector<unsigned char> secondPayload;
    std::vector<unsigned char> emptyFrame;
    std::vector<unsigned char> firstFrame;
    std::vector<unsigned char> secondFrame;
    std::vector<unsigned char> combined;
    std::vector<unsigned char> decoded;

    firstPayload.push_back(0x41);
    secondPayload.push_back(0x42);
    secondPayload.push_back(0x43);
    emptyFrame = AshTransport::encodeData(0x00, empty);
    firstFrame = AshTransport::encodeData(0x01, firstPayload);
    secondFrame = AshTransport::encodeData(0x02, secondPayload);

    /* A frame split at every meaningful boundary is not visible early. */
    transport.feed(&firstFrame[0], firstFrame.size() - 1);
    assert(!transport.popFrame(&decoded));
    transport.feed(&firstFrame[firstFrame.size() - 1], 1);
    assert(transport.popFrame(&decoded));
    assert(decoded.size() == 2 && decoded[0] == 0x01 && decoded[1] == 0x41);
    assert(!transport.popFrame(&decoded));

    /* Empty and back-to-back frames retain their ordering in one serial read. */
    combined.insert(combined.end(), emptyFrame.begin(), emptyFrame.end());
    combined.insert(combined.end(), firstFrame.begin(), firstFrame.end());
    combined.insert(combined.end(), secondFrame.begin(), secondFrame.end());
    transport.feed(&combined[0], combined.size());
    assert(transport.popFrame(&decoded));
    assert(decoded.size() == 1 && decoded[0] == 0x00);
    assert(transport.popFrame(&decoded));
    assert(decoded.size() == 2 && decoded[0] == 0x01 && decoded[1] == 0x41);
    assert(transport.popFrame(&decoded));
    assert(decoded.size() == 3 && decoded[0] == 0x02 && decoded[1] == 0x42 && decoded[2] == 0x43);
    assert(!transport.popFrame(&decoded));
}

static void testAshRejectsMalformedAndOverlongInput() {
    AshTransport transport;
    std::vector<unsigned char> payload;
    std::vector<unsigned char> encoded;
    std::vector<unsigned char> frame;
    std::vector<unsigned char> overlong(4097, 0x55);
    unsigned char strayDelimiter = 0x7e;
    unsigned char cancel = 0x1a;
    unsigned char malformedEscape[] = { 0x00, 0x7d, 0x00, 0x00, 0x00, 0x7e };

    /* Empty delimiters and invalid escaping are discarded without a frame. */
    transport.feed(&strayDelimiter, 1);
    transport.feed(malformedEscape, sizeof(malformedEscape));
    assert(!transport.popFrame(&frame));

    /* A reset drops a partial frame and oversized unterminated input recovers. */
    payload.push_back(0x55);
    encoded = AshTransport::encodeData(0x03, payload);
    transport.feed(&encoded[0], encoded.size() - 1);
    transport.feed(&cancel, 1);
    transport.feed(&encoded[encoded.size() - 1], 1);
    assert(!transport.popFrame(&frame));
    transport.feed(&overlong[0], overlong.size());
    transport.feed(&encoded[0], encoded.size());
    assert(transport.popFrame(&frame));
    assert(frame.size() == 2 && frame[0] == 0x03 && frame[1] == 0x55);
    assert(!transport.popFrame(&frame));
}

static void testDevicePersistence() {
    std::ostringstream path;
    path << "/tmp/microhad-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    std::string filename = directory + "/devices.json";
    std::string error;
    Device input;
    EndpointDescriptor endpoint;
    std::vector<Device> devices;
    std::vector<Device> loaded;
    assert(ensureDirectory(directory, &error));
    input.ieeeAddress = "0x00124b0000000001";
    input.zigbee.ensure().networkAddress = 0x1234;
    input.name = "Kitchen \"window\"\\\n";
    input.kind = "contact";
    input.contact.setPresent(true);
    input.contact.ensure().open = true;
    input.temperature.setPresent(true);
    input.temperature.ensure().celsius = 19.75;
    input.humidity.setPresent(true);
    input.humidity.ensure().percent = 47.25;
    input.occupancy.setPresent(true);
    input.occupancy.ensure().occupied = true;
    input.battery.setPresent(true);
    input.battery.ensure().percent = 76;
    input.light.setPresent(true);
    input.light.ensure().on = true;
    input.brightness.setPresent(true);
    input.brightness.ensure().percent = 63;
    input.remote.setPresent(true);
    input.zigbee.ensure().linkQuality = 91;
    input.lastSeen = 123456;
    input.zigbee.ensure().iasZoneId = 17;
    input.contactInitialAlertMinutes = 20;
    input.contactReminderAlertMinutes = 7;
    input.sensorRefreshIntervalMinutes = 17;
    input.manufacturerName = "eWeLink";
    input.modelId = "TH01";
    endpoint.endpoint = 1;
    endpoint.profileId = 0x0104;
    endpoint.deviceId = 0x0302;
    endpoint.deviceVersion = 1;
    endpoint.inputClusters.push_back(0x0000);
    endpoint.inputClusters.push_back(0x0001);
    endpoint.inputClusters.push_back(0x0402);
    endpoint.inputClusters.push_back(0x0405);
    endpoint.outputClusters.push_back(0x0003);
    input.zigbee.ensure().endpoints.push_back(endpoint);
    devices.push_back(input);
    Device nondimmable = input;
    nondimmable.ieeeAddress = "0x00124b0000000002";
    nondimmable.zigbee.ensure().networkAddress = 0x1235;
    nondimmable.brightness.setPresent(false);
    devices.push_back(nondimmable);
    Device matter;
    matter.ieeeAddress = "matter:0000000000000001:0000000000000002:0001";
    matter.external.ensure().externalId = matter.ieeeAddress;
    matter.external.ensure().component = "endpoint";
    matter.source = "matter";
    matter.kind = "switch";
    matter.matter.ensure().node = "0000000000000002";
    matter.matter.ensure().endpoint = 1;
    matter.matter.ensure().servers.push_back(6);
    matter.matter.ensure().servers.push_back(1037);
    matter.actor.setPresent(true);
    matter.co2.setPresent(true);
    matter.co2.ensure().known = true;
    matter.co2.ensure().ppm = 758.5;
    matter.co2.ensure().unit = "ppb";
    devices.push_back(matter);
    assert(saveDevices(filename, devices, &error));
    assert(loadDevices(filename, &loaded, &error));
    assert(loaded.size() == 3);
    assert(loaded[0].ieeeAddress == input.ieeeAddress);
    assert(loaded[0].zigbee.read().networkAddress == input.zigbee.read().networkAddress);
    assert(loaded[0].name == input.name && loaded[0].kind == input.kind);
    assert(loaded[0].contact.present() && loaded[0].contact.read().open && loaded[0].temperature.present());
    assert(loaded[0].temperature.read().celsius == input.temperature.read().celsius);
    assert(loaded[0].humidity.present() && loaded[0].humidity.read().percent == input.humidity.read().percent);
    assert(loaded[0].occupancy.present() && loaded[0].occupancy.read().occupied);
    assert(loaded[0].battery.present() && loaded[0].battery.read().percent == input.battery.read().percent);
    assert(loaded[0].light.present() && loaded[0].light.read().on && loaded[0].brightness.present() && loaded[0].brightness.read().percent == 63 && loaded[0].remote.present());
    assert(loaded[0].zigbee.read().linkQuality == input.zigbee.read().linkQuality && loaded[0].lastSeen == input.lastSeen);
    assert(loaded[0].zigbee.read().iasZoneId == input.zigbee.read().iasZoneId);
    assert(loaded[0].contactInitialAlertMinutes == 20 && loaded[0].contactReminderAlertMinutes == 7);
    assert(loaded[0].sensorRefreshIntervalMinutes == 17);
    assert(loaded[0].manufacturerName == "eWeLink" && loaded[0].modelId == "TH01");
    assert(loaded[0].zigbee.read().endpoints.size() == 1);
    assert(loaded[0].zigbee.read().endpoints[0].endpoint == 1 && loaded[0].zigbee.read().endpoints[0].profileId == 0x0104);
    assert(loaded[0].zigbee.read().endpoints[0].deviceId == 0x0302 && loaded[0].zigbee.read().endpoints[0].deviceVersion == 1);
    assert(loaded[0].zigbee.read().endpoints[0].inputClusters.size() == 4);
    assert(loaded[0].zigbee.read().endpoints[0].inputClusters[2] == 0x0402 && loaded[0].zigbee.read().endpoints[0].outputClusters[0] == 0x0003);
    assert(loaded[1].ieeeAddress == nondimmable.ieeeAddress && !loaded[1].brightness.present());
    assert(loaded[2].source == "matter" && loaded[2].external.read().externalId == matter.external.read().externalId);
    assert(loaded[2].matter.read().node == matter.matter.read().node && loaded[2].matter.read().endpoint == 1);
    assert(loaded[2].matter.read().servers.size() == 2 && loaded[2].matter.read().servers[0] == 6 &&
           loaded[2].matter.read().servers[1] == 1037);
    assert(loaded[2].actor.present() && loaded[2].co2.present() && loaded[2].co2.read().known &&
           loaded[2].co2.read().ppm == 758.5 && loaded[2].co2.read().unit == "ppb");
    unlink(filename.c_str());
    rmdir(directory.c_str());
}

static void testDeviceLoadRejectsCorruptDuplicatesAndNumbers() {
    std::ostringstream path;
    std::string error;
    std::vector<Device> loaded;
    path << "/tmp/microhad-zigbee-device-corrupt-" << static_cast<long>(getpid());
    assert(ensureDirectory(path.str(), &error));
    const std::string filename = path.str() + "/devices.json";
    {
        std::ofstream output(filename.c_str());
        assert(output);
        output << "{\"ieee_address\":\"ieee-a\",\"network_address\":4660,\"name\":\"first\"}\n";
        output << "{\"ieee_address\":\"ieee-a\",\"network_address\":4661,\"name\":\"duplicate ieee\"}\n";
        output << "{\"ieee_address\":\"ieee-b\",\"network_address\":4660,\"name\":\"duplicate node\"}\n";
        output << "{\"ieee_address\":\"bad-address\",\"network_address\":\"invalid\"}\n";
        output << "{\"ieee_address\":\"bad-range\",\"network_address\":65536}\n";
        output << "{\"ieee_address\":\"bad-brightness\",\"network_address\":4662,\"brightness_percent\":101}\n";
        output << "{\"ieee_address\":\"bad-temperature\",\"network_address\":4666,\"temperature_celsius\":\"not-a-number\"}\n";
        output << "{\"ieee_address\":\"bad-humidity\",\"network_address\":4667,\"humidity_percent\":101}\n";
        output << "not json\n";
        output << "{\"ieee_address\":\"ieee-c\",\"network_address\":4663,\"brightness_percent\":-1}\n";
        output.close(); assert(output);
    }
    assert(loadDevices(filename, &loaded, &error));
    assert(loaded.size() == 2 && loaded[0].ieeeAddress == "ieee-a" && loaded[0].name == "first" &&
           loaded[1].ieeeAddress == "ieee-c");
    Device sentinel; sentinel.ieeeAddress = "unchanged"; loaded.clear(); loaded.push_back(sentinel);
    {
        std::ofstream output(filename.c_str());
        assert(output);
        output << "{\"ieee_address\":\"valid-before-bad\",\"network_address\":4664}\n";
        output << "{\"ieee_address\":\"invalid-descriptor\",\"network_address\":4665,\"endpoint_descriptors\":\"01zz\"}\n";
        output.close(); assert(output);
    }
    error.clear();
    assert(!loadDevices(filename, &loaded, &error) && error == "invalid endpoint descriptor state");
    assert(loaded.size() == 1 && loaded[0].ieeeAddress == "unchanged");
    unlink(filename.c_str());
    loaded.push_back(Device());
    assert(loadDevices(path.str() + "/missing.json", &loaded, &error) && loaded.empty());
    rmdir(path.str().c_str());
}

static void testDeviceSettings() {
    std::ostringstream path;
    std::string error;
    std::vector<unsigned char> zone;
    std::vector<unsigned char> temperature;
    path << "/tmp/microhad-zigbee-settings-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    const std::string historyPath = directory + "/history.bin";
    Gateway gateway("/dev/null", 115200, directory, "", "", "", 0, historyPath);
    zone.push_back(1); zone.push_back(0);
    temperature.push_back(0x1a); temperature.push_back(0x09);
    assert(gateway.applyIasZoneStatus("0x00124b0000000004", zone));
    assert(gateway.applyAttributeReport("0x00124b0000000004", 0x0402, 0x0000, temperature));
    std::string historyJson;
    assert(gateway.historyStore()->readForDevice("zigbee:0x00124b0000000004", 0, &historyJson, &error));
    assert(historyJson.find(",1,23.299") != std::string::npos);
    assert(historyJson.find(",3,1,\"\"") != std::string::npos);
    assert(gateway.statusJson().find("\"contact_initial_alert_minutes\":10") != std::string::npos);
    assert(gateway.statusJson().find("\"contact_reminder_alert_minutes\":5") != std::string::npos);
    assert(gateway.statusJson().find("\"contact_trigger_mode\":false") != std::string::npos);
    assert(gateway.statusJson().find("\"contact_trigger_cooldown_seconds\":60") != std::string::npos);
    assert(gateway.statusJson().find("\"sensor_refresh_interval_minutes\":60") != std::string::npos);
    assert(gateway.setDeviceSettings("0x00124b0000000004", "Office window", true, 5, true, 30,
                                     true, 20, 7, true, 17, &error));
    assert(gateway.statusJson().find("\"name\":\"Office window\"") != std::string::npos);
    assert(gateway.statusJson().find("\"contact_initial_alert_minutes\":20") != std::string::npos);
    assert(gateway.statusJson().find("\"contact_reminder_alert_minutes\":7") != std::string::npos);
    assert(gateway.statusJson().find("\"sensor_refresh_interval_minutes\":17") != std::string::npos);
    assert(gateway.setDeviceSettings("0x00124b0000000004", "Office window", true, 5, true, 30,
                                     true, 20, 7, true, 17, &error, true, true, 12));
    assert(!GatewayTestPeer::hasContactReminder(gateway, "0x00124b0000000004"));
    assert(gateway.statusJson().find("\"contact_trigger_mode\":true") != std::string::npos);
    assert(gateway.statusJson().find("\"contact_trigger_cooldown_seconds\":12") != std::string::npos);
    assert(GatewayTestPeer::contactTriggerStatusExpires(&gateway, "0x00124b0000000004"));
    std::vector<Device> persistedDevices;
    assert(loadDevices(directory + "/devices.json", &persistedDevices, &error));
    assert(persistedDevices.size() == 1 && persistedDevices[0].contactTriggerMode &&
           persistedDevices[0].contactTriggerCooldownSeconds == 12);
    assert(!gateway.setDeviceSettings("0x00124b0000000004", "Office window", true, 5, true, 30,
                                      true, 20, 7, true, 17, &error, true, true, 0));
    assert(error == "invalid contact trigger cooldown");
    assert(gateway.statusJson().find("\"manufacturer_name\":\"\"") != std::string::npos);
    assert(gateway.statusJson().find("\"model_id\":\"\"") != std::string::npos);
    assert(gateway.setDeviceSettings("0x00124b0000000004", "Office window", true, 5, true, 30,
                                     true, 20, 7, true, 0, &error));
    assert(gateway.statusJson().find("\"sensor_refresh_interval_minutes\":0") != std::string::npos);
    assert(!gateway.setDeviceSettings("0x00124b0000000004", "Office window", true, 30, true, 5,
                                      true, 20, 7, true, 17, &error));
    assert(error == "invalid temperature thresholds");
    assert(!gateway.setDeviceSettings("0x00124b0000000004", "Office window", false, 0, false, 0,
                                      true, 0, 7, true, 0, &error));
    assert(error == "invalid contact notification times");
    assert(!gateway.setDeviceSettings("0xmissing", "Valid name", false, 0, false, 0,
                                      false, 0, 0, false, 0, &error));
    assert(error == "device not found");
    assert(!gateway.setDeviceSettings("0x00124b0000000004", "", false, 0, false, 0,
                                      true, 20, 7, true, 0, &error));
    assert(error == "device name must be between 1 and 64 characters");
    assert(!gateway.setDeviceSettings("0x00124b0000000004", std::string(65, 'x'), false, 0, false, 0,
                                      true, 20, 7, true, 0, &error));
    assert(error == "device name must be between 1 and 64 characters");
    double nan = std::numeric_limits<double>::quiet_NaN();
    assert(!gateway.setDeviceSettings("0x00124b0000000004", "Changed", true, nan, false, 0,
                                      true, 20, 7, true, 0, &error));
    assert(error == "invalid temperature thresholds");
    assert(!gateway.setDeviceSettings("0x00124b0000000004", "Changed", false, 0, false, 0,
                                      true, ULONG_MAX, 7, true, 0, &error));
    assert(error == "invalid contact notification times");
    assert(gateway.statusJson().find("\"name\":\"Office window\"") != std::string::npos);
    assert(gateway.statusJson().find("\"low_temperature_alert\":5") != std::string::npos);
    assert(gateway.statusJson().find("\"high_temperature_alert\":30") != std::string::npos);
    unlink((directory + "/devices.json").c_str());
    unlink(historyPath.c_str());
    unlink((historyPath + ".values").c_str());
    rmdir(directory.c_str());
}

static void testMatterInventoryIsCachedGroupedAndAtomic() {
    char directory[] = "/tmp/microhad-matter-inventory-XXXXXX";
    assert(mkdtemp(directory));
    const std::string nodeId="matter:0000000000000001:0000000000000002";
    std::string error;
    MatterEndpoint identity;
    identity.id=nodeId; identity.node="0000000000000002"; identity.endpoint=0xffff;
    identity.state="complete";
    {
        Gateway gateway("/dev/null",115200,directory,"","","");
        assert(gateway.reconcileMatterDevices(std::vector<MatterEndpoint>(1,identity),&error));
        assert(gateway.statusJson().find("\"id\":\""+nodeId+"\"")!=std::string::npos);
        assert(gateway.statusJson().find("\"endpoint_metadata_state\":\"missing\"")!=std::string::npos);
        assert(access((std::string(directory)+"/devices.json").c_str(),F_OK)!=0);
        MatterEndpoint root=identity; root.id=nodeId+":0000"; root.endpoint=0; root.servers.push_back(29); root.servers.push_back(47);
        MatterEndpoint sensor=root; sensor.id=nodeId+":0100"; sensor.endpoint=256;
        sensor.servers.push_back(1026); sensor.servers.push_back(0xffffffffU); sensor.clients.push_back(0xfffffffeU);
        identity.metadataComplete=true; identity.online=true;
        std::vector<MatterEndpoint> update; update.push_back(identity); update.push_back(root); update.push_back(sensor);
        assert(gateway.reconcileMatterDevices(update,&error));
        std::string status=gateway.statusJson();
        assert(status.find("\"endpoint_metadata_state\":\"ready\"")!=std::string::npos);
        assert(status.find("\"endpoint\":256")!=std::string::npos);
        assert(status.find("4294967295")!=std::string::npos && status.find("4294967294")!=std::string::npos);
        MatterReading battery; battery.node=identity.node; battery.endpoint=0; battery.cluster=47;
        battery.known=true; battery.numberValue=153;
        assert(gateway.applyMatterReading(battery,&error));
        assert(gateway.statusJson().find("\"has_battery\":true,\"battery_percent\":76")!=std::string::npos);
        // Exactly one common device; endpoint IDs remain nested aliases.
        assert(status.find("\"source\":\"matter\"")==status.rfind("\"source\":\"matter\""));
        identity.metadataComplete=false; identity.online=false;
        assert(gateway.reconcileMatterDevices(std::vector<MatterEndpoint>(1,identity),&error));
        assert(gateway.statusJson().find("\"endpoint\":256")!=std::string::npos);
        assert(gateway.statusJson().find("\"endpoint_metadata_state\":\"stale\"")!=std::string::npos);
        assert(gateway.statusJson().find("\"online\":false")!=std::string::npos);
        MatterReading report; report.node=identity.node; report.endpoint=256; report.cluster=1026;
        report.known=true; report.numberValue=21.5;
        assert(gateway.applyMatterReading(report,&error));
        assert(gateway.statusJson().find("\"online\":true")!=std::string::npos);
        for(int i=0;i<10;++i) { gateway.statusJson(); assert(gateway.reconcileMatterDevices(update,&error)); }
        // Coalesce all bootstrap/discovery updates until the scheduled flush.
        assert(access((std::string(directory)+"/devices.json").c_str(),F_OK)!=0);
    }
    {
        Gateway gateway("/dev/null",115200,directory,"","","");
        GatewayTestPeer::restoreState(gateway);
        std::string status=gateway.statusJson();
        assert(status.find("\"endpoint\":256")!=std::string::npos);
        assert(status.find("\"endpoint_metadata_state\":\"stale\"")!=std::string::npos);
        assert(status.find("\"online\":false")!=std::string::npos);
        struct stat before,after; const std::string path=std::string(directory)+"/devices.json";
        assert(stat(path.c_str(),&before)==0);
        identity.metadataComplete=true; identity.online=false;
        std::vector<MatterEndpoint> unchanged(1,identity);
        // Identity alone must never discard cached endpoints after restart.
        unchanged[0].metadataComplete=false;
        assert(gateway.reconcileMatterDevices(unchanged,&error));
        gateway.poll();
        assert(stat(path.c_str(),&after)==0 && before.st_ino==after.st_ino);
    }
    unlink((std::string(directory)+"/devices.json").c_str()); rmdir(directory);
}

static void testMatterAlertSettingsSurviveRestart() {
    char directory[] = "/tmp/microhad-matter-alert-restart-XXXXXX";
    assert(mkdtemp(directory) != 0);
    const std::string endpointId = "matter:0000000000000001:0000000000000002:0001";
    MatterEndpoint endpoint;
    endpoint.id = endpointId;
    endpoint.node = "0000000000000002";
    endpoint.endpoint = 1;
    endpoint.online = true;
    endpoint.servers.push_back(1026);
    endpoint.servers.push_back(1037);
    endpoint.servers.push_back(1066);
    std::vector<MatterEndpoint> endpoints(1, endpoint);
    std::string error;
    const std::string volatilePath = "/tmp/microhad-values.json";
    const std::string volatileBackup = volatilePath + ".matter-restart-test-backup";
    bool hadVolatileValues = rename(volatilePath.c_str(), volatileBackup.c_str()) == 0;
    {
        Gateway gateway("/dev/null", 115200, directory, "", "", "");
        assert(gateway.reconcileMatterDevices(endpoints, &error));
        DeviceValueAlert alert;
        alert.hasHigh = true;
        alert.high = 28.5;
        alert.hysteresis = 1.5;
        std::map<std::string, DeviceValueAlert> alerts;
        alerts["temperature"] = alert;
        MatterReading co2;
        co2.node = endpoint.node;
        co2.endpoint = endpoint.endpoint;
        co2.cluster = 1037;
        co2.known = true;
        co2.numberValue = 750;
        co2.unit = "ppb";
        assert(gateway.applyMatterReading(co2, &error));
        MatterReading pm25 = co2;
        pm25.cluster = 1066;
        pm25.numberValue = 11;
        pm25.unit = "ug/m3";
        assert(gateway.applyMatterReading(pm25, &error));
        DeviceValueAlert co2Alert;
        co2Alert.hasHigh = true;
        co2Alert.high = 800;
        co2Alert.hysteresis = 10;
        co2Alert.unit = "ppb";
        alerts["co2"] = co2Alert;
        DeviceValueAlert pm25Alert;
        pm25Alert.hasLow = true;
        pm25Alert.low = 10;
        pm25Alert.hysteresis = 1;
        pm25Alert.unit = "ug/m3";
        alerts["pm25"] = pm25Alert;
        assert(gateway.setDeviceValueAlerts(endpointId, alerts, &error));
        assert(gateway.statusJson().find("\"co2\":{\"has_low\":false,\"low\":0,\"has_high\":true,\"high\":800,\"hysteresis\":10,\"unit\":\"ppb\"") !=
               std::string::npos);
        co2Alert.unit = "ppm";
        alerts["co2"] = co2Alert;
        assert(!gateway.setDeviceValueAlerts(endpointId, alerts, &error));
        assert(error == "alert unit must match the current co2 measurement");
    }
    {
        std::vector<Device> parsed;
        assert(loadDevices(std::string(directory) + "/devices.json", &parsed, &error));
        assert(parsed.size() == 1 && parsed[0].source == "matter");
    }
    {
        Gateway restarted("/dev/null", 115200, directory, "", "", "");
        GatewayTestPeer::restoreState(restarted);
        std::vector<Device> loaded = restarted.devices();
        bool restoredBeforeSnapshot = false;
        for (unsigned long i = 0; i < loaded.size(); ++i)
            if (loaded[i].source == "matter" && loaded[i].external.read().externalId == endpointId &&
                loaded[i].valueAlerts.find("temperature") != loaded[i].valueAlerts.end()) restoredBeforeSnapshot = true;
        assert(restoredBeforeSnapshot);
        bool restoredReading = false;
        for (unsigned long i = 0; i < loaded.size(); ++i) {
            if (loaded[i].source == "matter" && loaded[i].external.read().externalId == endpointId) {
                restoredReading = loaded[i].co2.read().known && loaded[i].co2.read().ppm == 750 && loaded[i].co2.read().unit == "ppb" &&
                                  loaded[i].pm25.read().known && loaded[i].pm25.read().microgramsM3 == 11 &&
                                  loaded[i].pm25.read().unit == "ug/m3";
            }
        }
        assert(restoredReading);
        assert(restarted.reconcileMatterDevices(endpoints, &error));
        loaded = restarted.devices();
        bool restoredAfterSnapshot = false;
        for (unsigned long i = 0; i < loaded.size(); ++i) {
            if (loaded[i].source != "matter" || loaded[i].external.read().externalId != endpointId) continue;
            std::map<std::string, DeviceValueAlert>::const_iterator restored = loaded[i].valueAlerts.find("temperature");
            restoredAfterSnapshot = restored != loaded[i].valueAlerts.end() && restored->second.hasHigh &&
                                    restored->second.high == 28.5 && restored->second.hysteresis == 1.5;
        }
        assert(restoredAfterSnapshot);
        std::map<std::string, DeviceValueAlert>::const_iterator restoredCo2 = loaded[0].valueAlerts.find("co2");
        assert(restoredCo2 != loaded[0].valueAlerts.end() && restoredCo2->second.unit == "ppb" &&
               restoredCo2->second.high == 800);
        std::map<std::string, DeviceValueAlert>::const_iterator restoredPm25 = loaded[0].valueAlerts.find("pm25");
        assert(restoredPm25 != loaded[0].valueAlerts.end() && restoredPm25->second.unit == "ug/m3" &&
               restoredPm25->second.low == 10);
        MatterReading co2;
        co2.node = endpoint.node;
        co2.endpoint = endpoint.endpoint;
        co2.cluster = 1037;
        co2.known = true;
        co2.unit = "ppb";
        co2.numberValue = 760;
        assert(restarted.applyMatterReading(co2, &error));
        co2.numberValue = 810;
        assert(restarted.applyMatterReading(co2, &error));
        MatterReading pm25 = co2;
        pm25.cluster = 1066;
        pm25.unit = "ug/m3";
        pm25.numberValue = 11;
        assert(restarted.applyMatterReading(pm25, &error));
        pm25.numberValue = 8;
        assert(restarted.applyMatterReading(pm25, &error));
        loaded = restarted.devices();
        assert(loaded[0].valueAlerts["co2"].highActive);
        assert(loaded[0].valueAlerts["pm25"].lowActive);
    }
    unlink((std::string(directory) + "/devices.json").c_str());
    unlink((std::string(directory) + "/devices.json.tmp").c_str());
    assert(rmdir(directory) == 0);
    unlink(volatilePath.c_str());
    if (hadVolatileValues) assert(rename(volatileBackup.c_str(), volatilePath.c_str()) == 0);
}

static void testNetworkPersistence() {
    std::ostringstream path;
    std::string error;
    NetworkState input;
    NetworkState loaded;
    path << "/tmp/microhad-zigbee-network-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    std::string filename = directory + "/network.json";
    assert(ensureDirectory(directory, &error));
    input.formed = true; input.panId = 0x1a62; input.channel = 15;
    input.extendedPan.assign(8, 0x12); input.networkKey.assign(16, 0x34); input.preconfiguredKey.assign(16, 0x56);
    assert(saveNetworkState(filename, input, &error));
    {
        std::ifstream serialized(filename.c_str());
        std::string text((std::istreambuf_iterator<char>(serialized)), std::istreambuf_iterator<char>());
        assert(text.find("\"pan_id\":6754,\"channel\":15") != std::string::npos);
    }
    assert(loadNetworkState(filename, &loaded, &error));
    assert(loaded.formed && loaded.panId == input.panId && loaded.channel == input.channel);
    assert(loaded.extendedPan == input.extendedPan && loaded.networkKey == input.networkKey);
    assert(loaded.preconfiguredKey == input.preconfiguredKey);
    unlink(filename.c_str()); rmdir(directory.c_str());
}

static void testNetworkPersistenceRejectsInvalidState() {
    std::ostringstream path;
    std::string error;
    NetworkState loaded;
    path << "/tmp/microhad-zigbee-network-invalid-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    std::string filename = directory + "/network.json";
    assert(ensureDirectory(directory, &error));

    loaded.formed = true;
    loaded.panId = 0x1234;
    loaded.networkKey.assign(16, 0xa5);
    assert(loadNetworkState(filename, &loaded, &error));
    assert(!loaded.formed && loaded.panId == 0 && loaded.networkKey.empty());

    const char* invalid[] = {
        "{\"version\":1,\"formed\":true", /* truncated JSON */
        "{\"version\":1,\"formed\":true,\"pan_id\":4660,\"channel\":15,\"extended_pan\":\"00\",\"network_key\":\"0011\",\"preconfigured_key\":\"0011\"}",
        "{\"version\":1,\"formed\":true,\"pan_id\":70000,\"channel\":15,\"extended_pan\":\"1212121212121212\",\"network_key\":\"34343434343434343434343434343434\",\"preconfigured_key\":\"56565656565656565656565656565656\"}",
        "{\"version\":1,\"formed\":true,\"pan_id\":4660,\"channel\":15,\"extended_pan\":\"1212121212121212\",\"network_key\":\"zz343434343434343434343434343434\",\"preconfigured_key\":\"56565656565656565656565656565656\"}",
        "{\"version\":1,\"formed\":true,\"pan_id\":4660,\"channel\":15,\"extended_pan\":\"1212121212121212\",\"network_key\":\"34343434343434343434343434343434\"}"
    };
    for (unsigned long i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        std::ofstream output(filename.c_str());
        output << invalid[i];
        output.close();
        loaded.formed = true;
        loaded.panId = 0x1234;
        loaded.networkKey.assign(16, 0xa5);
        assert(!loadNetworkState(filename, &loaded, &error));
        assert(error == "invalid network state");
        assert(loaded.formed && loaded.panId == 0x1234 && loaded.networkKey.size() == 16 && loaded.networkKey[0] == 0xa5);
    }
    unlink(filename.c_str()); rmdir(directory.c_str());
}

static void testSceneConfigurationPersistence() {
    std::ostringstream path; std::string error; std::vector<SceneGroup> groups, loaded; std::vector<RemoteSceneBinding> bindings, loadedBindings;
    std::vector<NativeRemoteBinding> nativeBindings, loadedNativeBindings;
    path << "/tmp/microhad-zigbee-scenes-test-" << static_cast<long>(getpid());
    assert(ensureDirectory(path.str(), &error));
    SceneGroup group; Scene scene; SceneLightState state; RemoteSceneBinding binding;
    group.id=42; group.name="Kitchen"; group.members.push_back("0x00124b0000000042");
    scene.id=7; scene.name="Dinner"; state.ieeeAddress=group.members[0]; state.on=true; state.brightnessPercent=35; scene.states.push_back(state); group.scenes.push_back(scene);
    Scene secondScene; secondScene.id=8; secondScene.name="Late"; group.scenes.push_back(secondScene);
    groups.push_back(group);
    binding.ieeeAddress="0x00124b0000000099"; binding.action="on"; binding.groupId=42; binding.off=true; binding.sceneIds.push_back(7); binding.sceneIds.push_back(8); bindings.push_back(binding);
    NativeRemoteBinding native; native.ieeeAddress = binding.ieeeAddress; native.groupId = 42; nativeBindings.push_back(native);
    assert(saveSceneConfiguration(path.str()+"/scenes.conf",groups,bindings,nativeBindings,&error));
    assert(loadSceneConfiguration(path.str()+"/scenes.conf",&loaded,&loadedBindings,&loadedNativeBindings,&error));
    assert(loaded.size()==1 && loaded[0].id==42 && loaded[0].name=="Kitchen" && loaded[0].members.size()==1 && loaded[0].members[0]=="0x00124b0000000042");
    assert(loaded[0].scenes.size()==2 && loaded[0].scenes[0].id==7 && loaded[0].scenes[0].name=="Dinner" && loaded[0].scenes[0].states[0].brightnessPercent==35);
    assert(loaded[0].scenes[1].id==8 && loaded[0].scenes[1].name=="Late");
    assert(loadedBindings.size()==1 && loadedBindings[0].ieeeAddress=="0x00124b0000000099" && loadedBindings[0].action=="on" && loadedBindings[0].groupId==42 && loadedBindings[0].off && loadedBindings[0].sceneIds.size()==2 && loadedBindings[0].sceneIds[0]==7 && loadedBindings[0].sceneIds[1]==8);
    assert(loadedNativeBindings.size()==1 && loadedNativeBindings[0].ieeeAddress==native.ieeeAddress && loadedNativeBindings[0].groupId==42);
    unlink((path.str()+"/scenes.conf").c_str()); rmdir(path.str().c_str());
}

static bool saveStorageFixture(int kind, const std::string& filename, std::string* error) {
    if (kind == 0) {
        std::vector<Device> devices;
        return saveDevices(filename, devices, error);
    }
    if (kind == 1) {
        NetworkState network;
        network.formed = true; network.panId = 0x1234; network.channel = 15;
        network.extendedPan.assign(8, 0x12); network.networkKey.assign(16, 0x34);
        network.preconfiguredKey.assign(16, 0x56);
        return saveNetworkState(filename, network, error);
    }
    std::vector<SceneGroup> groups;
    std::vector<RemoteSceneBinding> bindings;
    return saveSceneConfiguration(filename, groups, bindings, error);
}

static void testStorageSaveFailuresPreserveDestinations() {
    std::ostringstream path;
    std::string error, content;
    path << "/tmp/microhad-zigbee-storage-failures-" << static_cast<long>(getpid());
    assert(ensureDirectory(path.str(), &error));
    for (int kind = 0; kind < 3; ++kind) {
        std::ostringstream name;
        name << path.str() << "/save-" << kind;
        std::string target = name.str();
        {
            std::ofstream previous(target.c_str());
            assert(previous); previous << "previous-state\n"; previous.close(); assert(previous);
        }
        assert(mkdir((target + ".tmp").c_str(), 0700) == 0);
        error.clear();
        assert(!saveStorageFixture(kind, target, &error) && !error.empty());
        assert(readTextFile(target, &content) && content == "previous-state\n");
        assert(rmdir((target + ".tmp").c_str()) == 0);

        assert(unlink(target.c_str()) == 0);
        assert(mkdir(target.c_str(), 0700) == 0);
        {
            std::ofstream previous((target + "/keep").c_str());
            assert(previous); previous << "directory-state\n"; previous.close(); assert(previous);
        }
        error.clear();
        assert(!saveStorageFixture(kind, target, &error) && !error.empty());
        assert(readTextFile(target + "/keep", &content) && content == "directory-state\n");
        assert(access((target + ".tmp").c_str(), F_OK) != 0);
        assert(unlink((target + "/keep").c_str()) == 0);
        assert(rmdir(target.c_str()) == 0);
    }
    {
        std::string nested = path.str() + "/missing/child";
        error.clear();
        assert(!ensureDirectory(nested, &error) && error.find("cannot create runtime directory") != std::string::npos);
        std::ofstream file((path.str() + "/not-a-directory").c_str());
        assert(file); file << "file"; file.close();
        error.clear();
        assert(!ensureDirectory(path.str() + "/not-a-directory", &error));
        unlink((path.str() + "/not-a-directory").c_str());
    }
    assert(rmdir(path.str().c_str()) == 0);
}

static void testSceneConfigurationMalformedRecordsAreAtomic() {
    std::ostringstream path; std::string error; std::vector<SceneGroup> groups; std::vector<RemoteSceneBinding> bindings;
    const char* malformed[] = {
        "G\t1\tRoom\nM\t1\tlight\nS\t1\t1\tScene\nT\t1\t1\tlight\tmaybe\t50\n",
        "G\tbad\tRoom\n", "G\t1\tRoom\textra\n", "M\t1\tlight\n", "S\t1\t256\tScene\n",
        "G\t65528\tRoom\n", "S\t1\t0\tScene\n", "T\t1\t1\tlight\t1\t101\n",
        "B\tremote\ton\t1\t2\t1\n", "B\tremote\tflash\t1\t0\t1\n", "X\t1\n",
        "G\t1\tRoom\nG\t1\tDuplicate\n", "G\t1\tRoom\nM\t1\tlight\nM\t1\tlight\n",
        "G\t1\tRoom\nS\t1\t1\tOne\nS\t1\t1\tDuplicate\n",
        "G\t1\tRoom\nS\t1\t1\tOne\nT\t1\t1\tlight\t1\t50\nT\t1\t1\tlight\t0\t20\n"
    };
    path << "/tmp/microhad-zigbee-scenes-malformed-" << static_cast<long>(getpid());
    assert(ensureDirectory(path.str(), &error));
    groups.push_back(SceneGroup()); groups[0].id = 77; groups[0].name = "unchanged";
    bindings.push_back(RemoteSceneBinding()); bindings[0].ieeeAddress = "unchanged";
    for (unsigned long i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        std::ofstream output((path.str() + "/scenes.conf").c_str());
        assert(output); output << malformed[i]; output.close(); assert(output);
        error.clear();
        assert(!loadSceneConfiguration(path.str() + "/scenes.conf", &groups, &bindings, &error));
        assert(error == "invalid scene configuration" && groups.size() == 1 && groups[0].id == 77 &&
               groups[0].name == "unchanged" && bindings.size() == 1 && bindings[0].ieeeAddress == "unchanged");
    }
    {
        std::ofstream output((path.str() + "/scenes.conf").c_str());
        assert(output); output << "# comment only\n\n"; output.close(); assert(output);
    }
    error.clear();
    assert(loadSceneConfiguration(path.str() + "/scenes.conf", &groups, &bindings, &error));
    assert(groups.empty() && bindings.empty());
    unlink((path.str()+"/scenes.conf").c_str()); rmdir(path.str().c_str());
}

static void testGenericCapabilities() {
    Device device;
    std::vector<unsigned char> temperature;
    std::vector<unsigned char> humidity;
    std::vector<unsigned char> xiaomi;
    std::vector<unsigned char> occupancy;
    std::vector<unsigned char> zone;
    std::vector<unsigned char> battery;
    device.ieeeAddress = "0x00124b0000000002";
    temperature.push_back(0x1a); temperature.push_back(0x09); /* 23.30 C */
    assert(applyZclAttribute(&device, 0x0402, 0x0000, temperature, 123));
    assert(device.temperature.present() && device.temperature.read().celsius == 23.3 && device.lastSeen == 123);
    humidity.push_back(0xb8); humidity.push_back(0x13); /* 50.48 % */
    assert(applyZclAttribute(&device, 0x0405, 0x0000, humidity, 123));
    assert(device.humidity.present() && device.humidity.read().percent == 50.48 && device.lastSeen == 123);
    xiaomi.push_back(8); /* length */
    xiaomi.push_back(0x64); xiaomi.push_back(0x29); xiaomi.push_back(0xa8); xiaomi.push_back(0x08); /* 22.16 C */
    xiaomi.push_back(0x65); xiaomi.push_back(0x21); xiaomi.push_back(0xb8); xiaomi.push_back(0x13); /* 50.48 % */
    device = Device();
    assert(applyZclAttribute(&device, 0x0000, 0xff01, xiaomi, 124));
    assert(device.temperature.present() && device.temperature.read().celsius == 22.16);
    assert(device.humidity.present() && device.humidity.read().percent == 50.48);
    {
        std::vector<unsigned char> temperatureOnly;
        std::vector<unsigned char> humidityOnly;
        std::vector<unsigned char> unknown;
        temperatureOnly.push_back(4); temperatureOnly.push_back(0x64); temperatureOnly.push_back(0x29);
        temperatureOnly.push_back(0xa8); temperatureOnly.push_back(0x08);
        humidityOnly.push_back(4); humidityOnly.push_back(0x65); humidityOnly.push_back(0x21);
        humidityOnly.push_back(0xb8); humidityOnly.push_back(0x13);
        unknown.push_back(4); unknown.push_back(0x99); unknown.push_back(0x21); unknown.push_back(1); unknown.push_back(0);
        device = Device();
        assert(applyZclAttribute(&device, 0x0000, 0xff01, temperatureOnly, 125));
        assert(device.temperature.present() && !device.humidity.present() && device.temperature.read().celsius == 22.16);
        device = Device();
        assert(applyZclAttribute(&device, 0x0000, 0xff01, humidityOnly, 126));
        assert(!device.temperature.present() && device.humidity.present() && device.humidity.read().percent == 50.48);
        device = Device();
        assert(!applyZclAttribute(&device, 0x0000, 0xff01, unknown, 127));
        assert(!device.temperature.present() && !device.humidity.present() && device.lastSeen == 0);
    }
    occupancy.push_back(1);
    assert(applyZclAttribute(&device, 0x0406, 0x0000, occupancy, 123));
    assert(device.occupancy.present() && device.occupancy.read().occupied && device.lastSeen == 123);
    {
        std::vector<unsigned char> response;
        response.push_back(0x00); response.push_back(0x00); /* measured value */
        response.push_back(0x00); /* success */
        response.push_back(0x29); /* signed 16-bit */
        response.push_back(0x1a); response.push_back(0x09); /* 23.30 C */
        assert(applyZclReadAttributeResponse(&device, 0x0402, response, 126));
        assert(device.temperature.read().celsius == 23.3 && device.lastSeen == 126);
    }
    battery.push_back(170);
    assert(applyZclAttribute(&device, 0x0001, 0x0021, battery, 124));
    assert(device.battery.present() && device.battery.read().percent == 85 && device.lastSeen == 124);
    zone.push_back(1); zone.push_back(0);
    assert(applyIasZoneStatus(&device, zone, 125));
    assert(device.contact.present() && device.contact.read().open && device.lastSeen == 125);
    zone[0] = 0;
    assert(applyIasZoneStatus(&device, zone, 126));
    assert(device.contact.present() && !device.contact.read().open && device.lastSeen == 126);

    device = Device();
    std::vector<unsigned char> waterType;
    waterType.push_back(0x2a); waterType.push_back(0x00); /* IAS water sensor */
    assert(applyZclAttribute(&device, 0x0500, 0x0001, waterType, 127));
    assert(device.waterLeak.present() && device.contact.present() && device.kind == "water leak");
    zone[0] = 1;
    assert(applyIasZoneStatus(&device, zone, 128));
    assert(device.waterLeak.read().known && device.waterLeak.read().detected && device.contact.read().open);
    zone[0] = 0;
    assert(applyIasZoneStatus(&device, zone, 129));
    assert(device.waterLeak.read().known && !device.waterLeak.read().detected && !device.contact.read().open);

    device = Device();
    std::vector<unsigned char> on(1, 1);
    std::vector<unsigned char> level(1, 127);
    assert(applyZclAttribute(&device, 0x0006, 0x0000, on, 126));
    assert(device.light.present() && device.light.read().on && device.lastSeen == 126);
    assert(applyZclAttribute(&device, 0x0008, 0x0000, level, 127));
    assert(device.brightness.present() && device.brightness.read().percent == 50 && device.lastSeen == 127);
    assert(isLightAttribute(0x0006, 0x0000) && isLightAttribute(0x0008, 0x0000));
    assert(!isLightAttribute(0x0008, 0x0001));
}

static void testZclRejectsMalformedAttributes() {
    Device device;
    std::vector<unsigned char> empty;
    std::vector<unsigned char> one(1, 0);
    std::vector<unsigned char> two(2, 0);
    std::vector<unsigned char> three(3, 0);
    std::vector<unsigned char> invalidHumidity;
    std::vector<unsigned char> invalidBattery(1, 201);
    std::vector<unsigned char> invalidOn(1, 2);
    std::vector<unsigned char> invalidLevel(1, 255);
    std::vector<unsigned char> wrongType;
    const unsigned short twoByteClusters[] = {0x0402, 0x0405, 0x0500};
    const unsigned short twoByteAttributes[] = {0x0000, 0x0000, 0x0002};
    const unsigned short oneByteClusters[] = {0x0406, 0x0001, 0x0006, 0x0008};
    const unsigned short oneByteAttributes[] = {0x0000, 0x0021, 0x0000, 0x0000};
    for (unsigned long i = 0; i < sizeof(twoByteClusters) / sizeof(twoByteClusters[0]); ++i) {
        assert(!applyZclAttribute(&device, twoByteClusters[i], twoByteAttributes[i], empty, 10));
        assert(!applyZclAttribute(&device, twoByteClusters[i], twoByteAttributes[i], one, 10));
        assert(!applyZclAttribute(&device, twoByteClusters[i], twoByteAttributes[i], three, 10));
    }
    for (unsigned long i = 0; i < sizeof(oneByteClusters) / sizeof(oneByteClusters[0]); ++i) {
        assert(!applyZclAttribute(&device, oneByteClusters[i], oneByteAttributes[i], empty, 10));
        assert(!applyZclAttribute(&device, oneByteClusters[i], oneByteAttributes[i], two, 10));
    }
    invalidHumidity.push_back(0x11); invalidHumidity.push_back(0x27); /* 100.01% */
    assert(!applyZclAttribute(&device, 0x0405, 0x0000, invalidHumidity, 10));
    assert(!applyZclAttribute(&device, 0x0001, 0x0021, invalidBattery, 10));
    assert(!applyZclAttribute(&device, 0x0006, 0x0000, invalidOn, 10));
    assert(!applyZclAttribute(&device, 0x0008, 0x0000, invalidLevel, 10));
    assert(!applyZclAttribute(&device, 0x9999, 0x0000, one, 10));
    assert(!applyIasZoneStatus(&device, empty, 10));
    assert(!applyIasZoneStatus(&device, three, 10));
    wrongType.push_back(0); wrongType.push_back(0); wrongType.push_back(0); /* attribute + success */
    wrongType.push_back(0x21); wrongType.push_back(0); wrongType.push_back(0); /* unsigned instead of signed temperature */
    assert(!applyZclReadAttributeResponse(&device, 0x0402, wrongType, 10));
    assert(!device.temperature.present() && !device.humidity.present() && !device.occupancy.present() && !device.battery.present() &&
           !device.contact.present() && !device.light.present() && device.lastSeen == 0);
}

static void testTh01SignedHumidity() {
    Device device;
    const unsigned char wire[] = {0x00, 0x00, 0x00, 0x29, 0xa1, 0x20}; /* captured 83.53% response */
    std::vector<unsigned char> response(wire, wire + sizeof(wire));
    assert(!applyZclReadAttributeResponse(&device, 0x0405, response, 100));
    device.manufacturerName = "eWeLink"; device.modelId = "TH01";
    unsigned observed = 0;
    assert(applyZclReadAttributeResponse(&device, 0x0405, response, 101, &observed));
    assert(device.humidity.present() && device.humidity.read().percent == 83.53 && device.lastSeen == 101);
    assert(observed == ZclObservedHumidity);
    response[4] = 0xff; response[5] = 0xff; /* negative/invalid */
    assert(!applyZclReadAttributeResponse(&device, 0x0405, response, 102));
    response[4] = 0x11; response[5] = 0x27; /* 100.01% */
    assert(!applyZclReadAttributeResponse(&device, 0x0405, response, 102));
    response.resize(5); /* truncated signed value */
    assert(!applyZclReadAttributeResponse(&device, 0x0405, response, 102));
    assert(device.humidity.read().percent == 83.53 && device.lastSeen == 101);
    response.assign(wire, wire + sizeof(wire)); response[3] = 0x21;
    assert(applyZclReadAttributeResponse(&device, 0x0405, response, 103));
}

static void testConfigureReportingEncoding() {
    std::vector<unsigned char> battery = buildZclConfigureReporting(7, 0x0021, 0x20, 3600, 65000, 1);
    std::vector<unsigned char> temperature = buildZclConfigureReporting(8, 0x0000, 0x29, 30, 3600, 20);
    assert(battery.size() == 12);
    assert(battery[1] == 7 && battery[6] == 0x20 && battery[11] == 1);
    assert(temperature.size() == 13);
    assert(temperature[1] == 8 && temperature[6] == 0x29 && temperature[11] == 20 && temperature[12] == 0);
}

static void testInterviewDescriptorAndPlanning() {
    GatewayTestPeer::cachedRejoinConfiguresReporting();
    GatewayTestPeer::th01HerdsmanReportingPlan();
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    std::vector<unsigned char> response;
    EndpointDescriptor descriptor;
    response.push_back(1); response.push_back(0); response.push_back(0x34); response.push_back(0x12);
    response.push_back(18); response.push_back(1); response.push_back(0x04); response.push_back(0x01);
    response.push_back(0x02); response.push_back(0x03); response.push_back(1); response.push_back(4);
    response.push_back(0x00); response.push_back(0x00); response.push_back(0x01); response.push_back(0x00);
    response.push_back(0x02); response.push_back(0x04); response.push_back(0x05); response.push_back(0x04);
    response.push_back(1); response.push_back(0x03); response.push_back(0x00);
    assert(GatewayTestPeer::parseSimpleDescriptor(gateway, response, &descriptor));
    assert(descriptor.endpoint == 1 && descriptor.profileId == 0x0104 && descriptor.deviceId == 0x0302);
    assert(descriptor.inputClusters.size() == 4 && descriptor.inputClusters[2] == 0x0402);
    GatewayTestPeer::buildConfiguration(&gateway, 0x1234, "0x00124b0000000010", descriptor);
    assert(gateway.statusJson().find("\"endpoint_descriptors\":[{\"endpoint\":1,\"profile_id\":260,\"device_id\":770,\"device_version\":1,\"input_clusters\":[0,1,1026,1029],\"output_clusters\":[3]}") != std::string::npos);
    assert(GatewayTestPeer::operationCount(gateway, 0x1234) == 10);
    assert(GatewayTestPeer::operationType(gateway, 0x1234, 0) == 3); /* InterviewBind */
    assert(GatewayTestPeer::operationCluster(gateway, 0x1234, 0) == 0x0402);
    assert(GatewayTestPeer::operationCluster(gateway, 0x1234, 1) == 0x0405);
    assert(GatewayTestPeer::operationCluster(gateway, 0x1234, 2) == 0x0001);
    {
        EndpointDescriptor remote;
        Gateway remoteGateway("/dev/null", 115200, "/tmp", "", "", "");
        remote.endpoint = 1;
        remote.outputClusters.push_back(0x0006);
        remote.outputClusters.push_back(0x0008);
        GatewayTestPeer::buildConfiguration(&remoteGateway, 0x1235, "0x00124b0000000012", remote);
        assert(GatewayTestPeer::operationCount(remoteGateway, 0x1235) == 2);
        assert(GatewayTestPeer::operationType(remoteGateway, 0x1235, 0) == 3);
        assert(GatewayTestPeer::operationCluster(remoteGateway, 0x1235, 0) == 0x0006);
        assert(GatewayTestPeer::operationType(remoteGateway, 0x1235, 1) == 3);
        assert(GatewayTestPeer::operationCluster(remoteGateway, 0x1235, 1) == 0x0008);
    }
    assert(GatewayTestPeer::pauseAndWake(&gateway, 0x1235));
    assert(GatewayTestPeer::retryKeepsTransactionSequence(&gateway, 0x1236));
    assert(GatewayTestPeer::contactCloseDuringInterview(&gateway));
    {
        std::ostringstream path;
        std::string error;
        path << "/tmp/microhad-zigbee-rejoin-test-" << static_cast<long>(getpid());
        assert(ensureDirectory(path.str(), &error));
        Gateway rejoinGateway("/dev/null", 115200, path.str(), "", "", "");
        assert(GatewayTestPeer::migrateRejoinedInterview(&rejoinGateway, 0x1237, 0x1238));
        unlink((path.str() + "/devices.json").c_str());
        unlink((path.str() + "/devices-state.json").c_str());
        rmdir(path.str().c_str());
    }
}

static void testJoinStatusJson() {
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    assert(gateway.statusJson().find("\"joining_preparing\":false") != std::string::npos);
}

static void testInterviewRetryDeadline() {
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    assert(GatewayTestPeer::interviewRetryContributesDeadline(&gateway, 0x2345));
}

static void testGatewayViewIsImmutable() {
    GatewayTestPeer::observedChannelSnapshot();
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    GatewayTestPeer::completeViewSchema(&gateway);
    GatewayView first = gateway.captureView();
    std::string status = first.statusJson();
    const char* fields[] = {"\"online\":", "\"network_forming\":", "\"network_formed\":", "\"serial_rx_bytes\":",
                            "\"ash_rx_frames\":", "\"joining_until\":", "\"joining_preparing\":", "\"notifications_enabled\":",
                            "\"devices\":[", "\"scene_groups\":[", "\"scene_bindings\":[", "\"endpoint_descriptors\":["};
    for (unsigned long i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) assert(status.find(fields[i]) != std::string::npos);
    assert(status.find("Kitchen \\\"window\\\"\\n") != std::string::npos);
    assert(first.logsJson().find("view control\\n\\u0001") != std::string::npos);
    std::vector<unsigned char> temperature;
    temperature.push_back(0x10); temperature.push_back(0x27); /* 100.00 C */
    assert(gateway.takeViewDirty());
    assert(gateway.applyAttributeReport("0x00124b00000000fe", 0x0402, 0x0000, temperature));
    GatewayView second = gateway.captureView();
    assert(first.statusJson().find("0x00124b00000000fe") == std::string::npos);
    assert(second.statusJson().find("0x00124b00000000fe") != std::string::npos);
    assert(gateway.takeViewDirty());
    assert(!gateway.takeViewDirty());
}

static void testRamLog() {
    std::string error;
    Gateway gateway("/dev/null", 115200, "/tmp", "", "", "");
    assert(!gateway.setJoining(300, &error));
    assert(error == "Zigbee coordinator is offline");
    assert(gateway.logsJson().find("Pairing request rejected: coordinator offline") != std::string::npos);
    GatewayTestPeer::logRingAndSerialization();
}

static void testFrameLogs() {
    assert(GatewayTestPeer::frameLogsContainDecodedDetails());
}

static void testRemoteKeyEvents() {
    assert(GatewayTestPeer::remoteKeyEventsAreLogged());
    GatewayTestPeer::tradfriFirmwareBindingAndOta();
    assert(GatewayTestPeer::nativeRemoteBindingFlow());
    assert(GatewayTestPeer::nativeIkeaFirmwareModes());
}

static void testStyrbarArrowEvents() {
    assert(GatewayTestPeer::styrbarArrowEventsAreLogged());
    assert(GatewayTestPeer::remoteBindingsAndIgnoredEvents());
}

static void testSceneConfigurationRejectsDuplicateStates() {
    assert(GatewayTestPeer::rejectsDuplicateSceneStates());
}

static void testSceneConfigurationRejectsInvalidReferences() {
    assert(GatewayTestPeer::rejectsInvalidSceneCrossReferences());
}

static void testSaveTriggeredProvisioning() {
    assert(GatewayTestPeer::saveTriggeredProvisioning());
}

static void testSceneBusyDoesNotAdvanceSelection() {
    assert(GatewayTestPeer::sceneBusyDoesNotAdvanceSelection());
    assert(GatewayTestPeer::sceneRuntimePersistence());
    assert(GatewayTestPeer::nativeAndFallbackSceneOutcomes());
    GatewayTestPeer::sceneBusyUnicastFallback();
}

static void testPendingSceneKeepsLatestRemoteAction() {
    assert(GatewayTestPeer::pendingSceneKeepsLatestRemoteAction());
    GatewayTestPeer::remoteCommandDeduplication();
#if MICROHAD_ZIGGURAT_ENABLED
    GatewayTestPeer::zigguratSceneUnicastFallback();
#endif
}

static void testContactNotificationIsDeferred() {
    std::ostringstream path;
    std::string error;
    std::vector<unsigned char> zone;
    std::vector<unsigned char> warm;
    std::vector<unsigned char> cold;
    path << "/tmp/microhad-zigbee-alert-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    NotificationWorker worker(directory, "", "", "");
    assert(worker.setEnabled(false, &error));
    Gateway gateway("/dev/null", 115200, directory, "", "", "", &worker);
    zone.push_back(1); zone.push_back(0);
    assert(gateway.applyIasZoneStatus("0x00124b0000000003", zone));
    assert(gateway.logsJson().find("Contact opened") == std::string::npos);
    assert(GatewayTestPeer::contactAlertIsCancelled(&gateway, "0x00124b0000000004"));
    warm.push_back(0xd0); warm.push_back(0x07); /* 20.00 C */
    cold.push_back(0xf4); cold.push_back(0x01); /* 5.00 C */
    assert(gateway.applyAttributeReport("0x00124b0000000005", 0x0402, 0x0000, warm));
    assert(gateway.setDeviceSettings("0x00124b0000000005", "Cold room", true, 10, false, 0,
                                     false, 0, 0, false, 0, &error));
    assert(gateway.applyAttributeReport("0x00124b0000000005", 0x0402, 0x0000, cold));
    assert(!gateway.sendTestNotification(&error));
    assert(error == "push notifications are disabled");
    assert(gateway.logsJson().find("Notification ") == std::string::npos);
    unlink((directory + "/notification-outbox.ndjson").c_str());
    unlink((directory + "/devices.json").c_str());
    rmdir(directory.c_str());
}

static void testTemperatureThresholdAlerts() {
    std::ostringstream path;
    std::string error;
    std::vector<unsigned char> temperature;
    path << "/tmp/microhad-zigbee-temperature-alert-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    NotificationWorker worker(directory, "", "", "");
    assert(worker.start(&error));
    Gateway gateway("/dev/null", 115200, directory, "", "", "", &worker);
    temperature.push_back(0xd0); temperature.push_back(0x07); /* 20.00 C */
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(gateway.setDeviceSettings("0x00124b0000000006", "Utility", true, 10, true, 30,
                                     false, 0, 0, false, 0, &error));
    temperature[0] = 0x1c; temperature[1] = 0x0c; /* 31.00 C */
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature high") == 1);
    temperature[0] = 0x80; temperature[1] = 0x0c; /* 32.00 C */
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature high") == 1);
    temperature[0] = 0xd0; temperature[1] = 0x07; /* back inside */
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification clear queued: legacy:0x00124b0000000006:temperature:high") == 1);
    temperature[0] = 0x1c; temperature[1] = 0x0c;
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature high") == 2);
    temperature[0] = 0x84; temperature[1] = 0x03; /* 9.00 C */
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature low") == 1);
    temperature[0] = 0x20; temperature[1] = 0x03; /* 8.00 C */
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature low") == 1);
    assert(gateway.setDeviceSettings("0x00124b0000000006", "Utility", false, 0, false, 0,
                                     false, 0, 0, false, 0, &error));
    temperature[0] = 0x1c; temperature[1] = 0x0c;
    assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature high") == 2);
    {
        std::map<std::string, DeviceValueAlert> alerts;
        DeviceValueAlert temperatureAlert;
        temperatureAlert.hasLow = true; temperatureAlert.low = 10;
        temperatureAlert.hasHigh = true; temperatureAlert.high = 30;
        temperatureAlert.hysteresis = 2;
        alerts["temperature"] = temperatureAlert;
        assert(gateway.setDeviceValueAlerts("0x00124b0000000006", alerts, &error));
        assert(gateway.statusJson().find("\"hysteresis\":2") != std::string::npos);
        temperature[0] = 0x54; temperature[1] = 0x0b; /* 29.00 C */
        assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
        temperature[0] = 0xb8; temperature[1] = 0x0b; /* 30.00 C */
        assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
        assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature high") == 3);
        temperature[0] = 0x84; temperature[1] = 0x03; /* 9.00 C */
        assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
        assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature low") == 2);
        temperature[0] = 0xe8; temperature[1] = 0x03; /* 10.00 C, still inside hysteresis band */
        assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
        assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature low") == 2);
        temperature[0] = 0xb0; temperature[1] = 0x04; /* 12.00 C, clear low state */
        assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
        assert(GatewayTestPeer::logOccurrences(gateway, "Notification clear queued: value:0x00124b0000000006:temperature:low") == 1);
        temperature[0] = 0xe8; temperature[1] = 0x03;
        assert(gateway.applyAttributeReport("0x00124b0000000006", 0x0402, 0x0000, temperature));
        assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Temperature low") == 3);
    }
    worker.stop();
    unlink((directory + "/devices.json").c_str());
    rmdir(directory.c_str());
}

static void testContactAlertReminders() {
    std::ostringstream path;
    std::string error;
    path << "/tmp/microhad-zigbee-contact-alert-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    NotificationWorker worker(directory, "", "", "");
    assert(worker.start(&error));
    Gateway gateway("/dev/null", 115200, directory, "", "", "", &worker);
    assert(GatewayTestPeer::contactAlertDeadlines(&gateway, "0x00124b0000000007"));
    assert(GatewayTestPeer::logOccurrences(gateway, "Notification queued: Window open") == 2);
    worker.stop();
    unlink((directory + "/devices.json").c_str());
    rmdir(directory.c_str());
}

static void testNotificationTogglePersistence() {
    std::ostringstream path;
    std::string error;
    path << "/tmp/microhad-zigbee-notification-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    {
        NotificationWorker worker(directory, "", "", "");
        assert(worker.enabled());
        assert(worker.setEnabled(false, &error));
        assert(!worker.enabled());
        assert(!worker.enqueue("Test", "Body", &error));
        assert(error == "notification worker is unavailable" || error == "push notifications are disabled");
    }
    {
        NotificationWorker worker(directory, "", "", "");
        assert(!worker.enabled());
        assert(worker.setEnabled(true, &error));
        assert(worker.enabled());
    }
    {
        std::string persisted;
        assert(readTextFile(directory + "/notifications.json", &persisted));
        assert(persisted == "{\"enabled\":true}\n");
        assert(access((directory + "/notifications.json.tmp").c_str(), F_OK) != 0);
    }
    {
        NotificationWorker worker(directory, "", "", "");
        assert(worker.enabled());
    }
    unlink((directory + "/notifications.json").c_str());
    rmdir(directory.c_str());
}

static void testPresenceBasedPushRouting() {
    {
        std::vector<NotificationWorker::RouteClient> clients;
        NotificationWorker::RouteClient client; client.id="phone-id"; client.label="Alice"; client.enabled=true;
        clients.push_back(client); client.id="browser-id"; client.label="Bob"; client.browser=true; clients.push_back(client);
        std::vector<std::string> requested,ids; std::string error;
        requested.push_back("alice"); requested.push_back("phone-id"); requested.push_back("BOB");
        assert(NotificationWorker::resolveRecipients(clients,requested,&ids,&error));
        assert(ids.size()==2 && ids[0]=="phone:phone-id" && ids[1]=="browser:browser-id");
        requested.push_back("missing"); assert(!NotificationWorker::resolveRecipients(clients,requested,&ids,&error)); assert(ids.empty());
        requested.assign(1,"Alice"); clients[1].label="ALICE";
        assert(!NotificationWorker::resolveRecipients(clients,requested,&ids,&error));
        requested.assign(1,"phone-id"); clients[1].label="phone-id";
        assert(NotificationWorker::resolveRecipients(clients,requested,&ids,&error)); assert(ids[0]=="phone:phone-id");
        clients[1].id="phone-id";
        assert(!NotificationWorker::resolveRecipients(clients,requested,&ids,&error));
    }

    std::vector<NotificationWorker::RouteClient> clients;
    NotificationWorker::RouteClient client;
    client.id = "phone-home"; client.presenceSensorId = "sensor-home"; client.enabled = true; clients.push_back(client);
    client.id = "browser-away"; client.presenceSensorId = "sensor-away"; clients.push_back(client);
    client.id = "phone-unassigned"; client.presenceSensorId = ""; clients.push_back(client);
    client.id = "disabled-home"; client.presenceSensorId = "sensor-home"; client.enabled = false; clients.push_back(client);
    std::vector<std::string> present, selected;
    present.push_back("sensor-home");
    selected = NotificationWorker::selectRecipients(clients, present);
    assert(selected.size() == 2 && selected[0] == "phone-home" && selected[1] == "phone-unassigned");
    present.clear();
    selected = NotificationWorker::selectRecipients(clients, present);
    assert(selected.size() == 3 && selected[0] == "phone-home" && selected[1] == "browser-away" && selected[2] == "phone-unassigned");
    present.push_back("unknown-sensor");
    selected = NotificationWorker::selectRecipients(clients, present);
    assert(selected.size() == 3);
    present.clear(); present.push_back("sensor-away");
    selected = NotificationWorker::selectRecipients(clients, present);
    assert(selected.size() == 2 && selected[0] == "browser-away" && selected[1] == "phone-unassigned");
}

static void testPushDeviceRegistry() {
    char directoryName[] = "/tmp/microhad-zigbee-push-devices-XXXXXX";
    char* directory = mkdtemp(directoryName);
    assert(directory != 0);
    const std::string id = "11111111-1111-4111-8111-111111111111";
    std::string error, before, after;
    struct stat firstStat, secondStat;
    {
        NotificationWorker worker(directory, "", "", "");
        assert(!worker.registerDevice("wrong", "Pixel", "token", &error));
        assert(worker.registerDevice(id, "Pixel 7", "token-one", &error));
        assert(readTextFile(std::string(directory) + "/push-devices.ndjson", &before));
        assert(stat((std::string(directory) + "/push-devices.ndjson").c_str(), &firstStat) == 0);
        assert((firstStat.st_mode & 0777) == 0600);
        assert(worker.registerDevice(id, "Pixel 7", "token-one", &error));
        assert(readTextFile(std::string(directory) + "/push-devices.ndjson", &after) && before == after);
        assert(stat((std::string(directory) + "/push-devices.ndjson").c_str(), &secondStat) == 0);
        assert(firstStat.st_ino == secondStat.st_ino);
        assert(worker.updateDevice(id, "Alice's phone", true, "sensor-hall", &error));
        assert(worker.updateDevice(id, "Alice's phone", false, &error));
        assert(worker.registerDevice(id, "Pixel 8", "token-two", &error));
        assert(worker.devicesJson().find("Alice's phone") != std::string::npos);
        assert(worker.devicesJson().find("\"enabled\":false") != std::string::npos);
        assert(worker.devicesJson().find("\"presence_sensor_id\":\"sensor-hall\"") != std::string::npos);
        assert(worker.devicesJson().find("token-two") == std::string::npos);
    }
    {
        NotificationWorker worker(directory, "", "", "");
        assert(worker.devicesJson().find("Alice's phone") != std::string::npos);
        assert(worker.devicesJson().find("Pixel 8") != std::string::npos);
        assert(worker.devicesJson().find("\"enabled\":false") != std::string::npos);
    }
    unlink((std::string(directory) + "/push-devices.ndjson").c_str());
    rmdir(directory);
}

class BlockingNotificationDelivery : public NotificationWorker::DeliveryHook {
public:
    BlockingNotificationDelivery() : entered_(false), released_(false), delivered_(0) {
        pthread_mutex_init(&mutex_, 0);
        pthread_cond_init(&changed_, 0);
    }
    ~BlockingNotificationDelivery() {
        pthread_cond_destroy(&changed_);
        pthread_mutex_destroy(&mutex_);
    }
    void deliver(const std::string&, const std::string&, const std::string& androidChannelId) {
        pthread_mutex_lock(&mutex_);
        ++delivered_;
        lastChannelId_ = androidChannelId;
        if (delivered_ == 1) {
            entered_ = true;
            pthread_cond_broadcast(&changed_);
            while (!released_) pthread_cond_wait(&changed_, &mutex_);
        }
        pthread_mutex_unlock(&mutex_);
    }
    bool waitUntilEntered() {
        struct timeval now;
        struct timespec deadline;
        gettimeofday(&now, 0);
        deadline.tv_sec = now.tv_sec + 2;
        deadline.tv_nsec = now.tv_usec * 1000;
        pthread_mutex_lock(&mutex_);
        while (!entered_) {
            int result = pthread_cond_timedwait(&changed_, &mutex_, &deadline);
            if (result != 0) { pthread_mutex_unlock(&mutex_); return false; }
        }
        pthread_mutex_unlock(&mutex_);
        return true;
    }
    void release() {
        pthread_mutex_lock(&mutex_);
        released_ = true;
        pthread_cond_broadcast(&changed_);
        pthread_mutex_unlock(&mutex_);
    }
    int delivered() {
        pthread_mutex_lock(&mutex_);
        int value = delivered_;
        pthread_mutex_unlock(&mutex_);
        return value;
    }
    std::string lastChannelId() {
        pthread_mutex_lock(&mutex_);
        std::string value = lastChannelId_;
        pthread_mutex_unlock(&mutex_);
        return value;
    }
private:
    pthread_mutex_t mutex_;
    pthread_cond_t changed_;
    bool entered_, released_;
    int delivered_;
    std::string lastChannelId_;
};

static void testNotificationWorkerQueueLifecycle() {
    char directoryName[] = "/tmp/microhad-zigbee-worker-XXXXXX";
    char* directory = mkdtemp(directoryName);
    std::string error;
    unsigned int i;
    assert(directory != 0);
    BlockingNotificationDelivery delivery;
    NotificationWorker worker(directory, "", "", "", &delivery);
    assert(worker.start(&error));
    assert(!worker.start(&error) && error == "notification worker already started");
    assert(worker.enqueue("first", "body", &error, "de.caser.garage.triggers"));
    assert(delivery.waitUntilEntered());
    assert(delivery.lastChannelId() == "de.caser.garage.triggers");
    for (i = 0; i < 32; ++i) assert(worker.enqueue("queued", "body", &error));
    assert(!worker.enqueue("overflow", "body", &error) && error == "notification queue is full");
    delivery.release();
    worker.stop();
    assert(delivery.delivered() == 33);
    worker.stop();
    assert(worker.start(&error));
    assert(worker.setEnabled(false, &error));
    assert(!worker.enqueue("disabled", "body", &error) && error == "push notifications are disabled");
    worker.stop();
    assert(worker.start(&error));
    worker.stop();
    unlink((std::string(directory) + "/notifications.json").c_str());
    rmdir(directory);
    fprintf(stderr, "unit: notification worker lifecycle and bounded queue\n");
}

static void testNotificationAuditDoesNotWriteRuntimeFlash() {
    std::ostringstream path;
    std::string error;
    path << "/tmp/microhad-zigbee-notification-audit-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    {
        NotificationWorker worker(directory, "", "", "");
        Gateway gateway("/dev/null", 115200, directory, "", "", "", &worker);
        assert(worker.start(&error));
        assert(gateway.sendTestNotification(&error));
        assert(access((directory + "/notification-outbox.ndjson").c_str(), F_OK) != 0);
    }
    rmdir(directory.c_str());
}

static std::string pushSubscription(const std::string& endpoint, char publicKeyTail) {
    std::string publicKey(65, '\0');
    std::string auth(16, '\0');
    publicKey[0] = 4;
    publicKey[64] = publicKeyTail;
    auth[15] = publicKeyTail;
    return "{\"endpoint\":\"" + endpoint + "\",\"keys\":{\"p256dh\":\"" + base64UrlEncode(publicKey) +
           "\",\"auth\":\"" + base64UrlEncode(auth) + "\"}}";
}

static void testBrowserPushSubscriptionBounds() {
    std::ostringstream path;
    std::string error, content;
    path << "/tmp/microhad-zigbee-push-test-" << static_cast<long>(getpid());
    std::string directory = path.str();
    assert(ensureDirectory(directory, &error));
    WebPushStore store(directory);
    assert(store.addSubscription(pushSubscription("https://push.example.test/a", 1), &error));
    assert(store.addSubscription(pushSubscription("https://push.example.test/a", 2), &error));
    assert(readTextFile(directory + "/web-push-subscriptions.ndjson", &content));
    assert(content.find("https://push.example.test/a") != std::string::npos && content.find("\n") == content.size() - 1);
    assert(content.find("https://push.example.test/a") == content.rfind("https://push.example.test/a"));
    assert(!store.addSubscription(pushSubscription("http://push.example.test/a", 3), &error));
    for (int i = 0; i < 31; ++i) {
        std::ostringstream endpoint;
        endpoint << "https://push.example.test/" << i;
        assert(store.addSubscription(pushSubscription(endpoint.str(), static_cast<char>(i + 4)), &error));
    }
    assert(!store.addSubscription(pushSubscription("https://push.example.test/overflow", 40), &error));
    assert(error == "Browser Push subscription limit reached");
    unlink((directory + "/web-push-subscriptions.ndjson").c_str());
    rmdir(directory.c_str());
}

static void testFcmMessageConstruction() {
    FirebaseServiceAccount account;
    std::string error;
    std::string json = "{\"project_id\":\"project\",\"client_email\":\"x@example.test\",\"private_key\":\"-----BEGIN PRIVATE KEY-----\\nKEY\\n-----END PRIVATE KEY-----\\n\"}";
    assert(parseFirebaseServiceAccount(json, &account, &error));
    assert(account.privateKeyPem == "-----BEGIN PRIVATE KEY-----\nKEY\n-----END PRIVATE KEY-----\n");
    assert(base64UrlEncode("foo") == "Zm9v");
    assert(base64UrlEncode("f") == "Zg");
    std::string decoded;
    assert(base64UrlDecode("Zm9v", &decoded) && decoded == "foo");
    assert(base64UrlDecode("Zg", &decoded) && decoded == "f");
    assert(makeFcmMessage("zigbee_alerts", "Window open", "Kitchen").find("\"topic\":\"zigbee_alerts\"") != std::string::npos);
}

template <typename T> static void compareJsonNumber(T value) {
    std::ostringstream expected; expected << value;
    JsonWriter actual; actual << value;
    assert(actual.str() == expected.str());
}

static void testApiFormattingAndProcStats() {
    compareJsonNumber(INT_MIN); compareJsonNumber(INT_MAX); compareJsonNumber(UINT_MAX);
    compareJsonNumber(LONG_MIN); compareJsonNumber(LONG_MAX); compareJsonNumber(ULONG_MAX);
    compareJsonNumber(std::numeric_limits<long long>::min());
    compareJsonNumber(std::numeric_limits<unsigned long long>::max());
    compareJsonNumber(static_cast<unsigned char>(42)); compareJsonNumber(static_cast<short>(-100));
    const double values[] = {0.0, -0.0, 1.23456789, -12.75, 0.0000001234567, 123456789.0,
        std::numeric_limits<double>::min(), std::numeric_limits<double>::max()};
    for (unsigned int i = 0; i < sizeof(values) / sizeof(values[0]); ++i) compareJsonNumber(values[i]);
    JsonWriter json; json << "{\"text\":\"" << jsonEscape("quotes\"\n") << "\",\"n\":" << -42 << '}';
    assert(json.str() == "{\"text\":\"quotes\\\"\\n\",\"n\":-42}");
    std::ostringstream stat;
    stat << "123 (worker (with) spaces) S";
    for (int field = 4; field <= 24; ++field) {
        if (field == 5) stat << " 777";
        else if (field == 14) stat << " 4294967297";
        else if (field == 15) stat << " 10";
        else if (field == 18) stat << " -20";
        else if (field == 22) stat << " 123456789012";
        else if (field == 24) stat << " 321";
        else stat << " 0";
    }
    ProcessStat parsed;
    assert(parseProcessStat(stat.str().c_str(), &parsed));
    assert(parsed.name == "worker (with) spaces" && parsed.group == 777 && parsed.rssPages == 321);
    assert(parsed.cpuTicks == 4294967307ULL && parsed.startTicks == 123456789012ULL);
    assert(!parseProcessStat("123 (broken) S 0", &parsed));
    assert(!parseProcessStat("123 broken S", &parsed));
    std::string invalid = stat.str();
    invalid.replace(invalid.find("4294967297"), 10, "18446744073709551616");
    assert(!parseProcessStat(invalid.c_str(), &parsed));
    invalid = stat.str(); invalid.replace(invalid.rfind("321"), 3, "-1");
    assert(!parseProcessStat(invalid.c_str(), &parsed));
}

static void testSharedJsonUtilities() {
    std::string value;
    long number = 0;
    assert(jsonStringField("{ \"value\" : \"quoted \\u005c\\\" value\\n\" }", "value", &value));
    assert(value == "quoted \\\" value\n");
    assert(jsonStringField("{\"value\":\"\\u20ac \\ud83d\\ude03\"}", "value", &value));
    assert(value == "\342\202\254 \360\237\230\203");
    assert(!jsonStringField("{\"value\":\"\\ud800\"}", "value", &value));
    assert(jsonLongField("{\"number\": 42}", "number", &number) && number == 42);
    assert(!jsonLongField("{\"number\": 42oops}", "number", &number));
    assert(!jsonStringField("{\"value\":\"first\",\"value\":\"second\"}", "value", &value));
    assert(!jsonStringField("{\"other\":\"value\"}", "value", &value));
    assert(!jsonStringField("{\"value\":\"value\"} trailing", "value", &value));
    assert(!jsonLongField("{\"number\":42} trailing", "number", &number));
    assert(jsonEscape("a\\b\"c\n\001") == "a\\\\b\\\"c\\n\\u0001");
}

static void testTrvzbProtocol() {
    puts("unit: TRVZB types, ranges, signed values, firmware gates and weekly schedules");
    Device d; d.modelId="TRVZB"; d.softwareBuildId="1.0.8"; d.normalizeComponents();
    std::vector<unsigned char> bytes; std::string error;
    assert(thermostatEncode(d,"occupied_heating_setpoint","21.5",&bytes,&error));
    assert(bytes.size()==5 && bytes[0]==0x12 && bytes[2]==0x29 && bytes[3]==0x66 && bytes[4]==8);
    assert(!thermostatEncode(d,"occupied_heating_setpoint","21.2",&bytes,&error));
    assert(!thermostatEncode(d,"occupied_heating_setpoint","nan",&bytes,&error));
    assert(!thermostatEncode(d,"system_mode","2",&bytes,&error));
    assert(!thermostatEncode(d,"temperature_sensor_select","2",&bytes,&error));
    std::vector<unsigned char> reconnected(1,3);
    assert(applyThermostatAttribute(&d,0xfc11,0x600e,0x20,reconnected,10));
    assert(d.thermostat.read().values[ThermostatField::temperature_sensor_select]==1);
    reconnected[0]=255;
    assert(applyThermostatAttribute(&d,0xfc11,0x6014,0x20,reconnected,10));
    assert(d.thermostat.read().values[ThermostatField::temporary_mode_select]==255);
    assert(!thermostatEncode(d,"valve_opening_degree","50",&bytes,&error));
    d.softwareBuildId="v1.3.0";
    assert(thermostatEncode(d,"valve_opening_degree","50",&bytes,&error));
    assert(thermostatEncode(d,"local_temperature_calibration","-1.2",&bytes,&error));
    assert(bytes[3]==244);
    std::vector<unsigned char> signedValue(1,244);
    assert(applyThermostatAttribute(&d,0x0201,0x10,0x28,signedValue,10));
    assert(d.thermostat.read().values[ThermostatField::local_temperature_calibration]==-1.2);
    assert(!applyThermostatAttribute(&d,0x0201,0x10,0x20,signedValue,10));
    signedValue.assign(2,0); signedValue[1]=0x80;
    assert(!applyThermostatAttribute(&d,0x0201,0,0x29,signedValue,10));
    signedValue[0]=0x34; signedValue[1]=8;
    assert(applyThermostatAttribute(&d,0x0201,0,0x29,signedValue,10));
    assert(d.temperature.read().observation.at==10 && d.temperature.read().celsius==21);
    assert(applyThermostatAttribute(&d,0x0201,0,0x29,signedValue,20));
    assert(d.temperature.read().observation.at==20);
    std::vector<unsigned char> on(1,1); assert(!applyZclAttribute(&d,6,0,on,21)); assert(!d.light.present());
    assert(thermostatSchedule("00:00/18 07:00/21.5 22:00/17",1,&bytes));
    assert(bytes.size()==15 && bytes[0]==3 && bytes[1]==2 && bytes[2]==1);
    assert(applyThermostatSchedule(&d,bytes));
    assert(d.thermostat.read().schedules.find(1)->second=="00:00/18 07:00/21.5 22:00/17");
    assert(!thermostatSchedule("07:00/20",1,&bytes));
    assert(!thermostatSchedule("00:00/18 00:00/20",1,&bytes));
    assert(!thermostatSchedule("00:00/18 24:00/20",1,&bytes));
    assert(!thermostatSchedule("00:00/18.1",1,&bytes));
    assert(!thermostatSchedule("00:00/18 01:00/20 02:00/20 03:00/20 04:00/20 05:00/20 06:00/20",1,&bytes));
    assert(!thermostatSchedule("",1,&bytes));
    d.thermostat.ensure().values[ThermostatField::smart_temperature_control]=2;
    assert(!thermostatEncode(d,"temperature_accuracy","-0.4",&bytes,&error));
    d.thermostat.ensure().unsupported.insert(ThermostatField::child_lock);
    assert(!thermostatEncode(d,"child_lock","1",&bytes,&error));
    assert(thermostatJson(d).find("\"schedules\":[null,\"00:00/18")!=std::string::npos);
}

static void testDeviceComponents() {
    puts("unit: optional device components and deep-copy snapshot isolation");
    Device empty;
    assert(!empty.temperature.present() && !empty.thermostat.present() && !empty.matter.present());
    assert(!empty.temperature.read().known && empty.temperature.read().celsius==0);
    assert(!empty.temperature.present() && !empty.thermostat.present());
    Device d;d.ieeeAddress="synthetic-components";d.temperature.ensure().known=true;d.temperature.ensure().celsius=0;
    d.thermostat.ensure().values[ThermostatField::occupied_heating_setpoint]=21;
    d.thermostat.ensure().schedules[0]="original";d.thermostat.ensure().pending.insert(ThermostatField::system_mode);
    EndpointDescriptor endpoint;endpoint.inputClusters.push_back(513);d.zigbee.ensure().endpoints.push_back(endpoint);
    d.matter.ensure().servers.push_back(513);d.external.ensure().component="endpoint";
#define COMPONENT_COPY(name) d.name.ensure();
    COMPONENT_COPY(humidity) COMPONENT_COPY(contact) COMPONENT_COPY(waterLeak) COMPONENT_COPY(airQuality)
    COMPONENT_COPY(co2) COMPONENT_COPY(pm25) COMPONENT_COPY(occupancy) COMPONENT_COPY(battery)
    COMPONENT_COPY(light) COMPONENT_COPY(brightness) COMPONENT_COPY(remote) COMPONENT_COPY(actor)
    COMPONENT_COPY(power) COMPONENT_COPY(wifiSignal)
#undef COMPONENT_COPY
    Device copy(d),assigned;assigned=d;
#define DISTINCT_COMPONENT(name) assert(&copy.name.read()!=&d.name.read() && &assigned.name.read()!=&d.name.read());
    DISTINCT_COMPONENT(temperature) DISTINCT_COMPONENT(thermostat) DISTINCT_COMPONENT(zigbee)
    DISTINCT_COMPONENT(matter) DISTINCT_COMPONENT(external) DISTINCT_COMPONENT(humidity) DISTINCT_COMPONENT(contact)
    DISTINCT_COMPONENT(waterLeak) DISTINCT_COMPONENT(airQuality) DISTINCT_COMPONENT(co2) DISTINCT_COMPONENT(pm25)
    DISTINCT_COMPONENT(occupancy) DISTINCT_COMPONENT(battery) DISTINCT_COMPONENT(light) DISTINCT_COMPONENT(brightness)
    DISTINCT_COMPONENT(remote) DISTINCT_COMPONENT(actor) DISTINCT_COMPONENT(power) DISTINCT_COMPONENT(wifiSignal)
#undef DISTINCT_COMPONENT
    copy.temperature.ensure().celsius=30;copy.thermostat.ensure().schedules[0]="changed";
    copy.zigbee.ensure().endpoints[0].inputClusters.clear();copy.matter.ensure().servers.clear();copy.external.ensure().component="changed";
    assert(d.temperature.read().celsius==0 && d.thermostat.read().schedules.find(0)->second=="original");
    assert(d.zigbee.read().endpoints[0].inputClusters.size()==1 && d.matter.read().servers.size()==1 && d.external.read().component=="endpoint");
    const Device& self=copy;copy=self;assert(copy.temperature.read().celsius==30);
    copy=empty;assert(!copy.temperature.present() && !copy.thermostat.present());
    Automation::SnapshotPtr snapshot=Automation::capture(d);
    d.temperature.ensure().celsius=35;d.thermostat.ensure().values[ThermostatField::occupied_heating_setpoint]=25;
    assert(snapshot->record->temperature.read().celsius==0);
    assert(snapshot->record->thermostat.read().values.get(ThermostatField::occupied_heating_setpoint,0)==21);
    char folderTemplate[]="/tmp/microhad-components-test-XXXXXX";assert(mkdtemp(folderTemplate));const std::string folder=folderTemplate;
    empty.ieeeAddress="saved-empty";empty.thermostatSourceId="saved-setting";empty.thermostatStaleMinutes=23;
    std::vector<Device> records(1,empty),loaded;std::string error;
    assert(saveDevices(folder+"/devices.json",records,&error) && loadDevices(folder+"/devices.json",&loaded,&error));
    assert(loaded.size()==1 && !loaded[0].temperature.present() && !loaded[0].thermostat.present() && !loaded[0].matter.present());
    assert(loaded[0].thermostatSourceId=="saved-setting" && loaded[0].thermostatStaleMinutes==23);
    assert(saveDeviceCache(folder+"/cache.pb",records,&error) && loadDeviceCache(folder+"/cache.pb",&loaded,&error));
    assert(loaded.size()==1 && !loaded[0].temperature.present() && !loaded[0].thermostat.present() && !loaded[0].matter.present());
    {
        Gateway gateway("/dev/null",115200,folder,"","","");
        MatterEndpoint endpoint;endpoint.id="synthetic-matter";endpoint.node="0000000000000002";endpoint.endpoint=1;
        endpoint.servers.push_back(1026);std::vector<MatterEndpoint> inventory(1,endpoint);
        assert(gateway.reconcileMatterDevices(inventory,&error));
        std::vector<Device> current=gateway.devices();assert(current.size()==1);
        assert(current[0].temperature.present() && !current[0].temperature.read().known);
        assert(!current[0].thermostat.present() && !current[0].contact.present() && !current[0].light.present() && !current[0].zigbee.present());
        MatterReading reading;reading.node=endpoint.node;reading.endpoint=1;reading.cluster=1026;reading.attribute=0;
        reading.known=true;reading.numberValue=0;
        assert(gateway.applyMatterReading(reading,&error));
        assert(gateway.reconcileMatterDevices(inventory,&error));current=gateway.devices();
        assert(current[0].temperature.present() && current[0].temperature.read().known && current[0].temperature.read().celsius==0);
        assert(!current[0].thermostat.present() && !current[0].actor.present());
        inventory[0].servers.clear();inventory[0].servers.push_back(6);
        assert(gateway.reconcileMatterDevices(inventory,&error));current=gateway.devices();
        assert(!current[0].temperature.present() && current[0].actor.present());
    }
    unlink((folder+"/devices.json").c_str());unlink((folder+"/cache.pb").c_str());rmdir(folder.c_str());
}

void runUnitTests() {
    {DeviceCacheIsolation cache;testDeviceComponents();}
    { DeviceCacheIsolation cache; GatewayTestPeer::deviceCacheBatching(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::genericThermostats(); }
    { DeviceCacheIsolation cache; testTrvzbProtocol(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::trvzb(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::deviceDeletion(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::reportingConfigurationPersistence(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::freshJoinRestoresConfiguration(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::gatewaySettings(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::emptyDeadlineIsUnscheduled(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::inactiveDeadlinesAreResetBeforeSelection(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::deadlineSelectionMatchesHandlerOrdering(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::ineligibleSensorRefreshCannotSpin(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::pausedRefreshWaitsForScheduledWake(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::pausedTimersDoNotWinDeadline(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::closingPermitReplyTimeout(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::refreshDiscoversMissingEndpoints(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::reviewAndTransportRegressions(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::ezspUpgradeCompatibility(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::networkJoinWindowDoesNotClearNetworkState(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::lightCommandEncoding(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::identifyCommandEncoding(); }
    { DeviceCacheIsolation cache; testAshRoundTrip(); }
    { DeviceCacheIsolation cache; testAshRejectsBadCrc(); }
    { DeviceCacheIsolation cache; testAshFragmentationAndFrameOrdering(); }
    { DeviceCacheIsolation cache; testAshRejectsMalformedAndOverlongInput(); }
    { DeviceCacheIsolation cache; testDevicePersistence(); }
    { DeviceCacheIsolation cache; testDeviceLoadRejectsCorruptDuplicatesAndNumbers(); }
    { DeviceCacheIsolation cache; testDeviceSettings(); }
    { DeviceCacheIsolation cache; testMatterInventoryIsCachedGroupedAndAtomic(); }
    { DeviceCacheIsolation cache; testMatterAlertSettingsSurviveRestart(); }
    { DeviceCacheIsolation cache; testNetworkPersistence(); }
    { DeviceCacheIsolation cache; testNetworkPersistenceRejectsInvalidState(); }
    { DeviceCacheIsolation cache; testSceneConfigurationPersistence(); }
    { DeviceCacheIsolation cache; testSceneConfigurationMalformedRecordsAreAtomic(); }
    { DeviceCacheIsolation cache; testStorageSaveFailuresPreserveDestinations(); }
    { DeviceCacheIsolation cache; testGenericCapabilities(); }
    { DeviceCacheIsolation cache; testZclRejectsMalformedAttributes(); }
    { DeviceCacheIsolation cache; testTh01SignedHumidity(); }
    { DeviceCacheIsolation cache; testConfigureReportingEncoding(); }
    { DeviceCacheIsolation cache; testInterviewDescriptorAndPlanning(); }
    { DeviceCacheIsolation cache; testJoinStatusJson(); }
    { DeviceCacheIsolation cache; testInterviewRetryDeadline(); }
    { DeviceCacheIsolation cache; testGatewayViewIsImmutable(); }
    { DeviceCacheIsolation cache; testRamLog(); }
    { DeviceCacheIsolation cache; testFrameLogs(); }
    { DeviceCacheIsolation cache; testRemoteKeyEvents(); }
    { DeviceCacheIsolation cache; testStyrbarArrowEvents(); }
    { DeviceCacheIsolation cache; testSceneConfigurationRejectsDuplicateStates(); }
    { DeviceCacheIsolation cache; testSceneConfigurationRejectsInvalidReferences(); }
    { DeviceCacheIsolation cache; testSaveTriggeredProvisioning(); }
    { DeviceCacheIsolation cache; testSceneBusyDoesNotAdvanceSelection(); }
    { DeviceCacheIsolation cache; testPendingSceneKeepsLatestRemoteAction(); }
    { DeviceCacheIsolation cache; testContactNotificationIsDeferred(); }
    { DeviceCacheIsolation cache; testTemperatureThresholdAlerts(); }
    { DeviceCacheIsolation cache; testContactAlertReminders(); }
    { DeviceCacheIsolation cache; GatewayTestPeer::contactAlertsSurviveRestart(); }
    { DeviceCacheIsolation cache; testNotificationTogglePersistence(); }
    { DeviceCacheIsolation cache; testPresenceBasedPushRouting(); }
    { DeviceCacheIsolation cache; testPushDeviceRegistry(); }
    { DeviceCacheIsolation cache; testNotificationWorkerQueueLifecycle(); }
    { DeviceCacheIsolation cache; testNotificationAuditDoesNotWriteRuntimeFlash(); }
    { DeviceCacheIsolation cache; testBrowserPushSubscriptionBounds(); }
    { DeviceCacheIsolation cache; testFcmMessageConstruction(); }
    { DeviceCacheIsolation cache; testApiFormattingAndProcStats(); }
    { DeviceCacheIsolation cache; testSharedJsonUtilities(); }
}
