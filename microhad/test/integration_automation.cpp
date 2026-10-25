#include "automation_worker.h"
#include "automation_types.h"
#include "gateway_runner.h"
#include "gateway.h"
#include "gateway_integration_peer.h"
#include "http_server.h"
#include "notification_worker.h"
#include "external_http_device.h"
#include "json_util.h"
#include "storage.h"
#include "matter_worker.h"
#include "matter_json.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>

class AutomationRunnerTestPeer {
public:
    static bool sharedInventory(GatewayRunner& runner,const std::string& id){
        bool same=false;pthread_mutex_lock(&runner.mutex_);
        std::map<std::string,Automation::SnapshotPtr>::const_iterator record=runner.automationCatalog_->devices.find(id);
        if(runner.currentView_&&record!=runner.automationCatalog_->devices.end()){
            const DeviceViewList& devices=runner.currentView_->view->devices;
            for(size_t i=0;i<devices.size();++i)if(devices[i].ieeeAddress==id)same=devices.record(i)==record->second->record;
        }
        pthread_mutex_unlock(&runner.mutex_);return same;
    }
    static void publish(GatewayRunner& runner){runner.publishAutomation(true);}
    static void queue(GatewayRunner& runner,const Automation::Action& action){runner.automationActions_.push_back(action);}
    static void drain(GatewayRunner& runner){
        Automation::Action action=runner.automationActions_.front();runner.automationActions_.pop_front();
        runner.executeAutomation(action);
    }
};

namespace {
using Automation::Value;
class Delivery:public NotificationWorker::DeliveryHook {
    pthread_mutex_t mutex_;unsigned count_;std::string key_,channel_;
public:
    Delivery():count_(0){pthread_mutex_init(&mutex_,0);}~Delivery(){pthread_mutex_destroy(&mutex_);}
    void deliver(const std::string&,const std::string&,const std::string&){pthread_mutex_lock(&mutex_);++count_;pthread_mutex_unlock(&mutex_);}
    void deliverState(const std::string&,const std::string&,const std::string& channel,const std::string& key,const std::string& action){assert(action=="show");pthread_mutex_lock(&mutex_);++count_;key_=key;channel_=channel;pthread_mutex_unlock(&mutex_);}
    unsigned count(){pthread_mutex_lock(&mutex_);unsigned n=count_;pthread_mutex_unlock(&mutex_);return n;}
    bool grouped(){pthread_mutex_lock(&mutex_);bool ok=key_.find("0x00124b0000000042:test")!=key_.npos&&channel_.empty();pthread_mutex_unlock(&mutex_);return ok;}
};
std::string encode(const std::string& s){std::string out;const char* hex="0123456789ABCDEF";for(size_t i=0;i<s.size();++i){unsigned char c=s[i];if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')out+=c;else{out+='%';out+=hex[c>>4];out+=hex[c&15];}}return out;}
std::string request(int port,const std::string& method,const std::string& target,const std::string& body=""){
    int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);sockaddr_in address;memset(&address,0,sizeof(address));address.sin_family=AF_INET;address.sin_port=htons(port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);timeval timeout={3,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));std::ostringstream wire;wire<<method<<" "<<target<<" HTTP/1.1\r\nHost: localhost\r\nContent-Length: "<<body.size()<<"\r\n\r\n"<<body;std::string bytes=wire.str();size_t at=0;while(at<bytes.size()){ssize_t n=write(fd,bytes.data()+at,bytes.size()-at);assert(n>0);at+=n;}std::string response;char buffer[4096];ssize_t n;while((n=read(fd,buffer,sizeof(buffer)))>0)response.append(buffer,n);close(fd);return response;
}
Value state(const std::string& root){std::string text;Value v;if(readTextFile(root+"/automations/state/main.json",&text))Automation::parse(text,&v);return v;}
bool until(const std::string& root,const std::string& key,double number){for(unsigned i=0;i<200;++i){if(state(root).fields[key]==Value(number))return true;usleep(10000);}return false;}
void clean(const std::string& path){DIR* d=opendir(path.c_str());assert(d);dirent* e;while((e=readdir(d))){std::string name=e->d_name;if(name=="."||name=="..")continue;std::string child=path+"/"+name;struct stat st;assert(lstat(child.c_str(),&st)==0);if(S_ISDIR(st.st_mode))clean(child);else unlink(child.c_str());}closedir(d);rmdir(path.c_str());}
}

namespace {
struct ThermostatPeer {
    int listener;
    unsigned commands;
    ThermostatPeer() : listener(-1), commands(0) {}
    static void* serve(void* opaque) {
        ThermostatPeer* peer=static_cast<ThermostatPeer*>(opaque);
        int fd=accept(peer->listener,0,0);assert(fd>=0);
        MatterWire::Decoder decoder;
        while(true) {
            unsigned char byte;ssize_t count=read(fd,&byte,1);
            if(count==0)break;
            assert(count==1&&decoder.feed(MatterWire::Bytes(1,byte)));
            MatterWire::Frame frame;if(!decoder.pop(&frame))continue;
            const std::string thread="{\"enabled\":true,\"available\":true,\"configured\":false,\"attached\":false,\"state\":\"detached\",\"linux_ipv6_or_tun\":false}";
            std::string reply;
            if(frame.opcode==9)reply="{\"schema\":1,\"ok\":true,\"result\":"+thread+"}";
            else if(frame.opcode==1)reply="{\"enabled\":true,\"available\":true,\"controller_initialized\":true,\"fabric_ready\":true,\"state\":\"running\"}";
            else if(frame.opcode==2)reply=thread;
            else if(frame.opcode==11) {
                const std::string body=MatterWire::jsonBody(frame.kind,frame.opcode,frame.payload);
                std::string command,node,field,value;long endpoint=0;
                assert(jsonStringField(body,"command",&command)&&command=="thermostat");
                assert(jsonStringField(body,"node",&node)&&node=="0000000000000072");
                assert(jsonLongField(body,"endpoint",&endpoint)&&endpoint==2);
                assert(jsonStringField(body,"field",&field)&&field=="system_mode");
                assert(jsonStringField(body,"value",&value)&&value=="4");++peer->commands;
                reply="{\"schema\":1,\"ok\":true,\"result\":{\"job\":\"0000000000000001\",\"state\":\"queued\"}}";
            } else {
                assert(frame.opcode==10);
                reply="{\"schema\":1,\"ok\":true,\"result\":{\"job\":\"0000000000000001\",\"state\":\"complete\",\"result\":{\"node\":\"0000000000000072\",\"endpoint\":2,\"cluster\":513,\"attribute\":28,\"value\":{\"type\":\"thermostat\",\"value\":4}}}}";
            }
            frame.kind=MatterWire::Frame::Response;assert(MatterWire::parseJson(frame.kind,frame.opcode,reply,&frame.payload));
            MatterWire::Bytes encoded;assert(MatterWire::encode(frame,&encoded));
            size_t offset=0;while(offset<encoded.size()){ssize_t sent=send(fd,&encoded[offset],encoded.size()-offset,MSG_NOSIGNAL);assert(sent>0);offset+=sent;}
        }
        close(fd);return 0;
    }
};

void automationControls(){
    puts("integration: Lua thermostat validation/actions/snapshots and scene selection queries");
    char directory[]="/tmp/microhad-automation-controls-XXXXXX";assert(mkdtemp(directory));
    const std::string root=directory;std::string error;
    {
        Gateway* gateway=new Gateway("/dev/null",115200,root,"","","",0,root+"/history.bin");
        GatewayRunner runner(gateway);
        Device thermostat;thermostat.ieeeAddress="trv";thermostat.name="Office thermostat";thermostat.source="zigbee";thermostat.zigbee.ensure().networkAddress=0x1234;
        EndpointDescriptor endpoint;endpoint.endpoint=1;endpoint.inputClusters.push_back(513);thermostat.zigbee.ensure().endpoints.push_back(endpoint);
        thermostat.thermostat.ensure().values[ThermostatField::control_sequence_of_operation]=2;
        thermostat.thermostat.ensure().values[ThermostatField::occupied_heating_setpoint]=20;
        thermostat.thermostat.ensure().values[ThermostatField::system_mode]=4;
        thermostat.thermostat.ensure().scheduleSupported=true;
        GatewayIntegrationTestPeer::automationDevice(gateway,thermostat);
        Device matter=thermostat;matter.ieeeAddress="matter-trv";matter.name="Matter thermostat";matter.source="matter";
        matter.matter.ensure().node="0000000000000072";matter.matter.ensure().endpoint=2;matter.matter.ensure().servers.push_back(513);
        assert(applyMatterThermostat(&matter,65531,"thermostat_list","[0,17,18,27,28,65532]",100));
        assert(applyMatterThermostat(&matter,65532,"thermostat","35",100));
        GatewayIntegrationTestPeer::automationDevice(gateway,matter);
        Device light;light.ieeeAddress="lamp";light.name="Lamp";light.light.setPresent(true);
        GatewayIntegrationTestPeer::automationDevice(gateway,light);
        SceneGroup group;group.id=7;group.name="Downstairs";Scene scene;scene.id=2;scene.name="Evening";group.scenes.push_back(scene);
        std::vector<SceneGroup> groups(1,group);GatewayIntegrationTestPeer::automationGroups(gateway,groups);
        AutomationRunnerTestPeer::publish(runner);
        AutomationWorker worker(root,&runner,0);assert(worker.start(&error));
        const std::string valid=
            "local g=assert(scenes.get('downstairs'));assert(g.id=='7' and g.name=='Downstairs' and g.scene_id==nil and g.off==nil);"
            "assert(not pcall(function()g.name='changed'end));assert(not scenes.get('missing'));"
            "local t=assert(devices.get('Office thermostat')).thermostat;assert(t.values.occupied_heating_setpoint==20);"
            "assert(not pcall(function()t.values.occupied_heating_setpoint=99 end));"
            "assert(devices.thermostat('trv','occupied_heating_setpoint',21));assert(devices.thermostat('trv','system_mode','4'));"
            "assert(devices.thermostat('trv','schedule_1','{\"mode\":1,\"transitions\":[{\"time\":0,\"heat\":20}]}'));"
            "assert(devices.thermostat('trv','schedule_1','__read__'));assert(devices.thermostat('matter-trv','occupied_cooling_setpoint',26));"
            "assert(not devices.thermostat('lamp','system_mode',4));assert(not devices.thermostat('missing','system_mode',4));"
            "assert(not devices.thermostat('trv','unknown',1));assert(not devices.thermostat('trv','system_mode',3));"
            "assert(not devices.thermostat('trv','occupied_heating_setpoint',100));assert(not devices.thermostat('trv','schedule_1','invalid'));"
            "assert(not devices.thermostat('trv','stale_minutes',4));assert(not devices.thermostat('trv','stale_minutes',5.5));"
            "assert(not devices.thermostat('matter-trv','child_lock',1));";
        if(!worker.validate("draft.lua",valid,&error)){fprintf(stderr,"controls validation: %s\n",error.c_str());assert(false);}
        assert(GatewayIntegrationTestPeer::automationThermostatOperations(*gateway)==0);
        assert(runner.acquireAutomationCatalog()->devices.find("matter-trv")->second->thermostat.fields.find("pending")->second.items.empty());
        assert(!worker.validate("draft.lua","devices.thermostat('trv','system_mode',true)",&error));
        assert(!worker.validate("draft.lua","devices.thermostat('trv','system_mode',math.huge)",&error));
        assert(!worker.validate("draft.lua","devices.thermostat('trv','system_mode',0/0)",&error));
        assert(!worker.validate("draft.lua","scenes.get(7)",&error));
        assert(worker.save("main.lua",
            "local old=scenes.get('7');persistent.changes=0;events.on({type='device.changed',device='trv',field='thermostat'},function(e)"
            "assert(e.old.values.occupied_heating_setpoint==20);assert(e.value.values.occupied_heating_setpoint==20);"
            "assert(e.value.error=='timeout');assert(old.scene_id==nil);persistent.changes=persistent.changes+1 end)",true,&error));
        thermostat.thermostat.ensure().error="timeout";GatewayIntegrationTestPeer::automationDevice(gateway,thermostat);
        AutomationRunnerTestPeer::publish(runner);assert(until(root,"changes",1));
        assert(worker.setEnabled("main.lua",false,&error));
        // Exercise the coordinator action path, including generation retirement.
        GatewayIntegrationTestPeer::setNetworkReady(gateway,true);
        Automation::Action action;action.operation="thermostat";action.target="trv";action.script="commands";action.generation=1;
        action.args=Value::object();action.args.fields["field"]=Value("occupied_heating_setpoint");action.args.fields["value"]=Value("21");
        runner.automationGeneration("commands",1);AutomationRunnerTestPeer::queue(runner,action);
        runner.automationGeneration("commands",2);AutomationRunnerTestPeer::drain(runner);
        assert(GatewayIntegrationTestPeer::automationThermostatOperations(*gateway)==0);
        action.generation=2;AutomationRunnerTestPeer::queue(runner,action);AutomationRunnerTestPeer::drain(runner);
        assert(GatewayIntegrationTestPeer::automationThermostatOperations(*gateway)==1);
        assert(worker.validate("draft.lua","local t=devices.get('trv').thermostat;assert(t.pending[1]=='occupied_heating_setpoint');assert(t.values.occupied_heating_setpoint==20)",&error));
        GatewayIntegrationTestPeer::clearAutomationThermostatOperations(gateway);
        action.target="matter-trv";action.args.fields["field"]=Value("occupied_cooling_setpoint");action.args.fields["value"]=Value("26");
        AutomationRunnerTestPeer::queue(runner,action);AutomationRunnerTestPeer::drain(runner);
        assert(worker.validate("draft.lua","local t=devices.get('matter-trv').thermostat;assert(#t.pending==0 and t.error=='Matter worker unavailable')",&error));
        MatterWorker unavailable(&runner,root+"/absent.sock");worker.setMatterWorker(&unavailable);
        AutomationRunnerTestPeer::queue(runner,action);AutomationRunnerTestPeer::drain(runner);
        assert(worker.validate("draft.lua","local t=devices.get('matter-trv').thermostat;assert(#t.pending==0 and t.error=='not_ready')",&error));
        worker.setMatterWorker(0);
        // Real recall completion updates the scene catalog without changing its configuration.
        GatewayIntegrationTestPeer::completeAutomationScene(gateway,7,2,false);AutomationRunnerTestPeer::publish(runner);
        assert(worker.validate("draft.lua","local g=scenes.get('7');assert(g.scene_id=='2' and g.scene_name=='Evening' and g.off==false)",&error));
        light.light.ensure().on=true;GatewayIntegrationTestPeer::automationDevice(gateway,light);AutomationRunnerTestPeer::publish(runner);
        assert(worker.validate("draft.lua","assert(scenes.get('7').scene_id=='2')",&error));
        GatewayIntegrationTestPeer::completeAutomationScene(gateway,7,0,true);AutomationRunnerTestPeer::publish(runner);
        assert(worker.validate("draft.lua","local g=scenes.get('7');assert(g.off==true and g.scene_id=='2')",&error));
        groups[0].scenes[0].name="Renamed";GatewayIntegrationTestPeer::automationGroups(gateway,groups);AutomationRunnerTestPeer::publish(runner);
        assert(worker.validate("draft.lua","assert(scenes.get('7').scene_name=='Renamed')",&error));
        groups[0].scenes.clear();GatewayIntegrationTestPeer::automationGroups(gateway,groups);AutomationRunnerTestPeer::publish(runner);
        assert(worker.validate("draft.lua","local g=scenes.get('7');assert(g.scene_id==nil and g.scene_name==nil and g.off==true)",&error));
        SceneGroup duplicate=group;duplicate.id=8;duplicate.name="downstairs";groups.push_back(duplicate);
        GatewayIntegrationTestPeer::automationGroups(gateway,groups);AutomationRunnerTestPeer::publish(runner);
        assert(worker.validate("draft.lua","assert(not scenes.get('Downstairs'));assert(scenes.get('7').id=='7');assert(scenes.get('8').id=='8')",&error));
        groups[1].name="7";GatewayIntegrationTestPeer::automationGroups(gateway,groups);AutomationRunnerTestPeer::publish(runner);
        assert(worker.validate("draft.lua","assert(scenes.get('7').id=='7')",&error));
        thermostat.name="Matter thermostat";GatewayIntegrationTestPeer::automationDevice(gateway,thermostat);AutomationRunnerTestPeer::publish(runner);
        assert(worker.validate("draft.lua","assert(not devices.thermostat('Matter thermostat','system_mode',4));assert(devices.thermostat('trv','system_mode',4))",&error));
        groups.clear();GatewayIntegrationTestPeer::automationGroups(gateway,groups);AutomationRunnerTestPeer::publish(runner);
        assert(worker.validate("draft.lua","assert(not scenes.get('7'))",&error));
        worker.stop();
    }
    clean(root);
}
}

void runAutomationIntegrationTests(){
    automationControls();
    puts("integration: automation HTTP workflow, coordinator readings/actions, source watcher and restart");
    char path[]="/tmp/microhad-automation-integration-XXXXXX";assert(mkdtemp(path));std::string root=path,error;
    int serial[2];assert(pipe(serial)==0);
    {
        // No coordinator thread: deterministically retire an already queued
        // entry before executing the same dequeue path used by the runner.
        Gateway* gateway=new Gateway("/dev/null",115200,root,"","","",0,root+"/retired-history.bin");
        GatewayRunner runner(gateway);Automation::Action action;
        action.operation="refresh";action.target="missing";action.script="queued";action.generation=1;
        const std::string before=gateway->logsJson();
        runner.automationGeneration(action.script,1);AutomationRunnerTestPeer::queue(runner,action);
        runner.automationGeneration(action.script,0);AutomationRunnerTestPeer::drain(runner);
        assert(gateway->logsJson()==before);
        runner.automationGeneration(action.script,1);AutomationRunnerTestPeer::queue(runner,action);
        runner.automationGeneration(action.script,2);AutomationRunnerTestPeer::drain(runner);
        assert(gateway->logsJson()==before);
        action.generation=2;AutomationRunnerTestPeer::queue(runner,action);AutomationRunnerTestPeer::drain(runner);
        assert(gateway->logsJson()!=before);
    }
    {
        Delivery delivery;NotificationWorker notifications(root,"","","",&delivery);assert(notifications.start(&error));
        Device lamp;lamp.ieeeAddress="0x00124b0000000042";lamp.name="Lamp";lamp.source="zigbee";lamp.light.setPresent(true);lamp.brightness.setPresent(true);lamp.kind="light";lamp.zigbee.ensure().networkAddress=0x1234;
        EndpointDescriptor endpoint;endpoint.endpoint=1;endpoint.inputClusters.push_back(6);endpoint.inputClusters.push_back(8);lamp.zigbee.ensure().endpoints.push_back(endpoint);
        Device duplicate=lamp;duplicate.ieeeAddress="0x00124b0000000043";duplicate.zigbee.ensure().networkAddress=0x1235;duplicate.name="lamp";std::vector<Device> fixtures;fixtures.push_back(lamp);fixtures.push_back(duplicate);assert(saveDevices(root+"/devices.json",fixtures,&error));
        SceneGroup group;group.id=1;group.name="Office";Scene scene;scene.id=2;scene.name="Evening";group.scenes.push_back(scene);assert(saveSceneConfiguration(root+"/scenes.conf",std::vector<SceneGroup>(1,group),std::vector<RemoteSceneBinding>(),&error));
        Gateway* gateway=new Gateway("/dev/null",115200,root,"","","",&notifications,root+"/history.bin");GatewayIntegrationTestPeer::attachSerial(gateway,serial[0]);
        GatewayRunner runner(gateway);ExternalHttpDeviceWorker external(root,&runner);gateway->setShellyTransport(&external);assert(runner.start(&error));
        std::vector<int> inputs(1,0),temperatures(1,0),relays(1,0);
        assert(runner.registerShelly("1234567890abcdef","Office","Office","uni",inputs,temperatures,relays,&error));
        std::string externalId;assert(external.saveDevice("1234567890abcdef","Office","192.0.2.1","192.0.2.2","uni","","",inputs,temperatures,relays,30,false,true,false,&externalId,&error));
        AutomationWorker automations(root,&runner,&notifications);assert(automations.start(&error));
        HttpServer server(0,&runner,root,"","",&external,&notifications,0,"",&automations);assert(server.start(&error));int port=server.boundPort();
        std::string code="persistent.values=0;persistent.changes=0;local catalog=devices.list({source='shelly'});assert(#catalog==3,'catalog size');local sensor;local actor;for _,d in ipairs(catalog)do if d.kind=='temperature'then sensor=d.id end;if d.kind=='relay'then actor=d.id end end;assert(sensor,'sensor');assert(actor,'actor');"
          "events.on({type='device.value',device=sensor,field='temperature_celsius'},function(e)assert(e.device.temperature_celsius==e.value);persistent.values=persistent.values+1;assert(devices.actor(actor,true));end);"
          "events.on({type='device.changed',device=sensor,field='temperature_celsius'},function(e)persistent.changes=persistent.changes+1 end);"
          "events.on({type='device.removed',source='shelly'},function(e)persistent.removed=(persistent.removed or 0)+1 end);"
          "events.on({type='device.added',source='matter'},function(e)assert(e.device.id==e.device_id);persistent.added=(persistent.added or 0)+1 end);"
          "events.on({type='device.offline',source='matter'},function(e)assert(e.device.online==false);persistent.offline=(persistent.offline or 0)+1 end);"
          "events.on({type='device.online',source='matter'},function(e)assert(e.device.online==true);persistent.online=(persistent.online or 0)+1 end);";
        // A local setting exercises the real Lua command queue without radio traffic.
        MatterEndpoint trv;trv.id="matter:0000000000000001:0000000000000072:0002";trv.node="0000000000000072";trv.endpoint=2;trv.online=true;trv.servers.push_back(513);
        assert(runner.reconcileMatterDevices(std::vector<MatterEndpoint>(1,trv),&error));
        MatterReading features;features.node=trv.node;features.endpoint=2;features.cluster=513;features.attribute=65532;features.known=true;features.unit="thermostat";features.textValue="1";
        assert(runner.applyMatterReading(features,&error));
        features.attribute=28;features.textValue="0";assert(runner.applyMatterReading(features,&error));
        assert(automations.save("command.lua","assert(devices.thermostat('"+trv.id+"','system_mode',4))",true,&error));
        bool failed=false;
        for(unsigned i=0;i<200&&!failed;++i){
            Automation::CatalogPtr catalog=runner.acquireAutomationCatalog();
            std::map<std::string,Automation::SnapshotPtr>::const_iterator found=catalog->devices.find(trv.id);
            failed=found!=catalog->devices.end()&&found->second->thermostat.fields.find("error")!=found->second->thermostat.fields.end()&&found->second->thermostat.fields.find("error")->second==Value("Matter worker unavailable");
            if(!failed)usleep(10000);
        }
        assert(failed);assert(automations.remove("command.lua",&error));
        const std::string socketPath=root+"/matter.sock";
        ThermostatPeer peer;peer.listener=socket(AF_UNIX,SOCK_STREAM,0);assert(peer.listener>=0);
        sockaddr_un address;memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;assert(socketPath.size()<sizeof(address.sun_path));strcpy(address.sun_path,socketPath.c_str());
        assert(bind(peer.listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
        assert(chmod(socketPath.c_str(),0600)==0&&listen(peer.listener,1)==0);
        pthread_t service;assert(pthread_create(&service,0,ThermostatPeer::serve,&peer)==0);
        {
            MatterWorker connected(&runner,socketPath);automations.setMatterWorker(&connected);assert(connected.start(&error));
            std::string status;bool ready=false;
            for(unsigned i=0;i<200&&!ready;++i){ready=connected.status(false,&status,&error);if(!ready)usleep(10000);}
            assert(ready);
            assert(automations.save("command.lua","assert(devices.thermostat('"+trv.id+"','system_mode',4))",true,&error));
            bool confirmed=false;
            for(unsigned i=0;i<300&&!confirmed;++i){
                Automation::CatalogPtr catalog=runner.acquireAutomationCatalog();
                Value t=catalog->devices.find(trv.id)->second->thermostat;
                confirmed=t.fields["values"].fields["system_mode"]==Value(4.)&&t.fields["pending"].items.empty()&&t.fields["error"]==Value("");
                if(!confirmed)usleep(10000);
            }
            assert(confirmed);assert(automations.remove("command.lua",&error));automations.setMatterWorker(0);
            connected.stop();
        }
        assert(pthread_join(service,0)==0&&peer.commands==1);close(peer.listener);assert(unlink(socketPath.c_str())==0);
        assert(runner.removeMatterNode(trv.node,&error));
        std::string save="filename=main.lua&source="+encode(code);
        std::string response=request(port,"POST","/api/automations/save",save);assert(response.find("200 OK")!=response.npos);
        if(automations.listJson().find("\"loaded\":true")==std::string::npos){fprintf(stderr,"%s\n%s\n",automations.listJson().c_str(),automations.logsJson("").c_str());assert(false);}
        Automation::CatalogPtr before=runner.acquireAutomationCatalog();
        std::string sensorId;for(std::map<std::string,Automation::SnapshotPtr>::const_iterator it=before->devices.begin();it!=before->devices.end();++it)if(it->second->metadata->source=="shelly"&&it->second->metadata->kind=="temperature")sensorId=it->first;
        assert(!sensorId.empty());
        assert(runner.applyShellyEvent("1234567890abcdef",2,0,18,&error));
        Automation::CatalogPtr changed=runner.acquireAutomationCatalog();
        assert(before!=changed&&before->names==changed->names);
        assert(AutomationRunnerTestPeer::sharedInventory(runner,sensorId));
        assert(changed->devices.find(sensorId)->second->record->temperature.read().celsius==18);
        assert(before->devices.find(sensorId)->second->metadata==changed->devices.find(sensorId)->second->metadata);
        assert(before->devices.find("0x00124b0000000042")->second==changed->devices.find("0x00124b0000000042")->second);
        assert(!before->devices.find(sensorId)->second->has(Automation::Temperature));
        assert(changed->devices.find(sensorId)->second->values[Automation::Temperature]==18);
        assert(runner.applyShellyEvent("1234567890abcdef",2,0,18,&error));assert(runner.applyShellyEvent("1234567890abcdef",2,0,19,&error));assert(until(root,"values",3));assert(until(root,"changes",2));
        assert(changed->devices.find(sensorId)->second->values[Automation::Temperature]==18);
        assert(changed->devices.find(sensorId)->second->record->temperature.read().celsius==18);
        const std::string oldName=changed->devices.find(sensorId)->second->metadata->name;
        assert(runner.setDeviceSettings(sensorId,"Renamed temperature",false,0,false,0,false,0,0,false,0,&error));
        Automation::CatalogPtr renamed=runner.acquireAutomationCatalog();
        assert(renamed->names!=changed->names&&renamed->resolve("rENAMED TEMPERATURE",&error));
        assert(!renamed->resolve(oldName,&error));
        assert(AutomationRunnerTestPeer::sharedInventory(runner,sensorId));
        assert(renamed->devices.find("0x00124b0000000042")->second==changed->devices.find("0x00124b0000000042")->second);
        assert(notifications.registerDevice("11111111-1111-4111-8111-111111111111","Phone","test-token",&error));
        assert(notifications.updateDevice("11111111-1111-4111-8111-111111111111","Test phone",true,&error));
        const std::string apiChecks="local d,err=devices.get('LaMp');assert(d==nil and err:find('ambiguous'));local lamp=assert(devices.get('0x00124b0000000042'));assert(lamp.name=='Lamp');assert(not pcall(function()lamp.name='changed'end));assert(devices.light(lamp.id,{on=true,brightness=50}));assert(devices.refresh(lamp.id));assert(scenes.activate('office','evening'));assert(scenes.off('1'));local ok=devices.actor(lamp.id,true);assert(ok==nil);assert(not pcall(function()devices.light(lamp.id,{brightness=101})end));assert(not pcall(function()devices.list({unsupported=true})end));assert(not pcall(function()devices.actor(lamp.id,1)end));assert(not pcall(function()notify.send(1,'body')end));assert(not pcall(function()notify.send('title','body',{bad=true})end));assert(not devices.get('missing'));assert(not scenes.activate('missing','scene'));assert(not scenes.activate('office','missing'));assert(notify.send('Automation','Message',{tag='test',device_id=lamp.id,recipients={'Test phone','11111111-1111-4111-8111-111111111111'}}));";
        std::string checkedApis=apiChecks;for(size_t i=0;i<checkedApis.size();++i)if(checkedApis[i]==';')checkedApis.insert(++i,"\n");
        const unsigned delivered=delivery.count();if(!automations.validate("apis.lua",checkedApis,&error)){fprintf(stderr,"API checks: %s\n",error.c_str());assert(false);}usleep(20000);assert(delivery.count()==delivered);
        assert(automations.save("apis.lua",checkedApis,true,&error));for(unsigned i=0;i<100&&delivery.count()==delivered;++i)usleep(10000);assert(delivery.count()==delivered+1);assert(delivery.grouped());
        assert(automations.save("unknown-recipient.lua","local ok,err=notify.send('Title','Body',{recipients={'Test phone','missing'}});assert(ok==nil and err:find('unknown notification recipient'));persistent.checked=true",true,&error));
        assert(notifications.setEnabled(false,&error));assert(automations.validate("disabled.lua","assert(notify.send('Title','Body'))",&error));assert(automations.save("disabled.lua","local ok,err=notify.send('Title','Body');assert(ok==nil and err);persistent.disabled=1",true,&error));assert(notifications.setEnabled(true,&error));
        Automation::Action stale;stale.operation="refresh";stale.script="retired";stale.generation=1;assert(!runner.submitAutomation(stale,&error));
        assert(request(port,"GET","/api/automations/main.lua").find("persistent.values")!=std::string::npos);
        assert(request(port,"GET","/api/automations/logs").find("record_id")!=std::string::npos);
        assert(request(port,"POST","/api/automations/validate","filename=main.lua&source="+encode("persistent.values=999;for _,d in ipairs(devices.list({source='shelly'}))do if d.kind=='relay'then assert(devices.actor(d.id,true))end end")).find("200 OK")!=std::string::npos);
        assert(state(root).fields["values"].number==3);
        // Node reachability is authoritative; endpoint inventories deliberately
        // retain recent report reachability for a sixty-second grace period.
        MatterEndpoint matter;matter.id="matter:0000000000000001:0000000000000030:ffff";matter.node="0000000000000030";matter.endpoint=0xffff;matter.online=true;
        assert(runner.reconcileMatterDevices(std::vector<MatterEndpoint>(1,matter),&error));assert(until(root,"added",1));
        matter.online=false;assert(runner.reconcileMatterDevices(std::vector<MatterEndpoint>(1,matter),&error));assert(until(root,"offline",1));
        matter.online=true;assert(runner.reconcileMatterDevices(std::vector<MatterEndpoint>(1,matter),&error));assert(until(root,"online",1));
        assert(runner.removeShelly("1234567890abcdef",&error));assert(until(root,"removed",3));
        assert(request(port,"POST","/api/automations/validate","filename=main.lua&source="+encode("while true do end")).find("400 Bad Request")!=std::string::npos);
        assert(request(port,"POST","/api/automations/disable","filename=main.lua").find("200 OK")!=std::string::npos);
        assert(automations.listJson().find("main.lua.disabled")!=std::string::npos);
        assert(request(port,"POST","/api/automations/save","filename=main.lua.disabled&source="+encode("persistent.boots=(persistent.boots or 0)+1")).find("200 OK")!=std::string::npos);
        assert(request(port,"POST","/api/automations/enable","filename=main.lua.disabled").find("200 OK")!=std::string::npos);assert(until(root,"boots",1));
        assert(request(port,"POST","/api/automations/reload","filename=main.lua").find("200 OK")!=std::string::npos);assert(until(root,"boots",2));
        // SSH-style atomic replacement, including same-size/same-second edits.
        std::string next=root+"/automations/.ssh-next";FILE* f=fopen(next.c_str(),"w");assert(f);fputs("persistent.ssh=1",f);fclose(f);assert(rename(next.c_str(),(root+"/automations/main.lua").c_str())==0);usleep(350000);assert(!state(root).fields.count("ssh"));
        assert(request(port,"POST","/api/automations/refresh","").find("200 OK")!=std::string::npos);assert(until(root,"ssh",1));
        const Value refreshed=state(root);
        const std::string refreshedList=automations.listJson();
        assert(request(port,"POST","/api/automations/refresh","").find("200 OK")!=std::string::npos);
        assert(state(root)==refreshed);assert(automations.listJson()==refreshedList);
        f=fopen(next.c_str(),"w");assert(f);fputs("persistent.ssh=2",f);fclose(f);assert(rename(next.c_str(),(root+"/automations/main.lua").c_str())==0);assert(request(port,"POST","/api/automations/refresh","").find("200 OK")!=std::string::npos);assert(until(root,"ssh",2));
        assert(request(port,"POST","/api/automations/save","filename=../evil.lua&source=x").find("400 Bad Request")!=std::string::npos);
        assert(request(port,"POST","/api/automations/save","filename=large.lua.disabled&source="+encode("--"+std::string(32000,'x'))).find("200 OK")!=std::string::npos);
        assert(request(port,"POST","/api/automations/reset","filename=main.lua").find("200 OK")!=std::string::npos);assert(!state(root).fields.count("values"));
        assert(request(port,"POST","/api/automations/delete","filename=main.lua").find("200 OK")!=std::string::npos);assert(state(root).fields.empty());
        automations.stop();runner.stop();notifications.stop();
    }
    close(serial[1]);clean(root);
}
