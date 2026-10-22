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
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

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

void runAutomationIntegrationTests(){
    puts("integration: automation HTTP workflow, coordinator readings/actions, source watcher and restart");
    char path[]="/tmp/microhad-automation-integration-XXXXXX";assert(mkdtemp(path));std::string root=path,error;
    int serial[2];assert(pipe(serial)==0);
    {
        Delivery delivery;NotificationWorker notifications(root,"","","",&delivery);assert(notifications.start(&error));
        Device lamp;lamp.ieeeAddress="0x00124b0000000042";lamp.name="Lamp";lamp.source="zigbee";lamp.hasLight=true;lamp.hasBrightness=true;lamp.kind="light";lamp.networkAddress=0x1234;
        EndpointDescriptor endpoint;endpoint.endpoint=1;endpoint.inputClusters.push_back(6);endpoint.inputClusters.push_back(8);lamp.endpoints.push_back(endpoint);
        Device duplicate=lamp;duplicate.ieeeAddress="0x00124b0000000043";duplicate.networkAddress=0x1235;duplicate.name="lamp";std::vector<Device> fixtures;fixtures.push_back(lamp);fixtures.push_back(duplicate);assert(saveDevices(root+"/devices.json",fixtures,&error));
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
        std::string save="filename=main.lua&source="+encode(code);
        std::string response=request(port,"POST","/api/automations/save",save);assert(response.find("200 OK")!=response.npos);
        if(automations.listJson().find("\"loaded\":true")==std::string::npos){fprintf(stderr,"%s\n%s\n",automations.listJson().c_str(),automations.logsJson("").c_str());assert(false);}
        assert(runner.applyShellyEvent("1234567890abcdef",2,0,18,&error));assert(runner.applyShellyEvent("1234567890abcdef",2,0,18,&error));assert(runner.applyShellyEvent("1234567890abcdef",2,0,19,&error));assert(until(root,"values",3));assert(until(root,"changes",2));
        const std::string apiChecks="local d,err=devices.get('LaMp');assert(d==nil and err:find('ambiguous'));local lamp=assert(devices.get('0x00124b0000000042'));assert(lamp.name=='Lamp');assert(not pcall(function()lamp.name='changed'end));assert(devices.light(lamp.id,{on=true,brightness=50}));assert(devices.refresh(lamp.id));assert(scenes.activate('office','evening'));assert(scenes.off('1'));local ok=devices.actor(lamp.id,true);assert(ok==nil);assert(not pcall(function()devices.light(lamp.id,{brightness=101})end));assert(not pcall(function()devices.list({unsupported=true})end));assert(not pcall(function()devices.actor(lamp.id,1)end));assert(not pcall(function()notify.send(1,'body')end));assert(not pcall(function()notify.send('title','body',{bad=true})end));assert(not devices.get('missing'));assert(not scenes.activate('missing','scene'));assert(not scenes.activate('office','missing'));assert(notify.send('Automation','Message',{tag='test',device_id=lamp.id}));";
        std::string checkedApis=apiChecks;for(size_t i=0;i<checkedApis.size();++i)if(checkedApis[i]==';')checkedApis.insert(++i,"\n");
        const unsigned delivered=delivery.count();if(!automations.validate("apis.lua",checkedApis,&error)){fprintf(stderr,"API checks: %s\n",error.c_str());assert(false);}usleep(20000);assert(delivery.count()==delivered);
        assert(automations.save("apis.lua",checkedApis,true,&error));for(unsigned i=0;i<100&&delivery.count()==delivered;++i)usleep(10000);assert(delivery.count()==delivered+1);assert(delivery.grouped());
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
        std::string next=root+"/automations/.ssh-next";FILE* f=fopen(next.c_str(),"w");assert(f);fputs("persistent.ssh=1",f);fclose(f);assert(rename(next.c_str(),(root+"/automations/main.lua").c_str())==0);assert(until(root,"ssh",1));
        f=fopen(next.c_str(),"w");assert(f);fputs("persistent.ssh=2",f);fclose(f);assert(rename(next.c_str(),(root+"/automations/main.lua").c_str())==0);assert(until(root,"ssh",2));
        assert(request(port,"POST","/api/automations/save","filename=../evil.lua&source=x").find("400 Bad Request")!=std::string::npos);
        assert(request(port,"POST","/api/automations/save","filename=large.lua.disabled&source="+encode("--"+std::string(32000,'x'))).find("200 OK")!=std::string::npos);
        assert(request(port,"POST","/api/automations/reset","filename=main.lua").find("200 OK")!=std::string::npos);assert(!state(root).fields.count("values"));
        assert(request(port,"POST","/api/automations/delete","filename=main.lua").find("200 OK")!=std::string::npos);assert(state(root).fields.empty());
        automations.stop();runner.stop();notifications.stop();
    }
    close(serial[1]);clean(root);
}
