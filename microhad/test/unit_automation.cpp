#include "automation_worker.h"
#include "automation_types.h"
#include "gateway.h"
#include "json_util.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using Automation::Value;
void typedSnapshots(){
    Device d;d.ieeeAddress="sensor";d.name="Sensor";d.hasTemperature=true;d.temperatureKnown=true;d.temperatureCelsius=18;
    Automation::SnapshotPtr before=Automation::capture(d);
    assert(Automation::capture(d,before)==before);
    d.temperatureCelsius=19;Automation::SnapshotPtr changed=Automation::capture(d,before);
    assert(changed!=before&&changed->metadata==before->metadata);
    assert(before->values[Automation::Temperature]==18&&changed->values[Automation::Temperature]==19);
    d.name="Renamed";Automation::SnapshotPtr renamed=Automation::capture(d,changed);
    assert(renamed->metadata!=changed->metadata&&changed->metadata->name=="Sensor");
    EndpointDescriptor endpoint;endpoint.inputClusters.push_back(8);d.endpoints.push_back(endpoint);
    Automation::SnapshotPtr capability=Automation::capture(d,renamed);
    assert(capability->metadata!=renamed->metadata&&capability->metadata->brightness);
    assert(!renamed->metadata->brightness&&!capability->has(Automation::Brightness));
    Automation::Event e;e.type="device.value";e.device=before;e.field=Automation::Temperature;e.chain=123;e.depth=2;e.at=42;
    assert(e.data.fields.empty());Value event=Automation::eventValue(e);
    assert(event.fields["device"].fields["temperature_celsius"].number==18&&event.fields["value"].number==18);
    assert(event.fields["chain_id"].string=="123"&&event.fields["chain_depth"].number==2);
    e.type="device.changed";e.device=changed;e.hasOld=true;e.old=Value(18.);
    event=Automation::eventValue(e);assert(event.fields["old"].number==18&&event.fields["value"].number==19);
    d.temperatureKnown=false;Automation::SnapshotPtr unknown=Automation::capture(d,capability);
    assert(!unknown->has(Automation::Temperature)&&!Automation::snapshot(*unknown).fields.count("temperature_celsius"));
}

void schedulerRegressions(){
    lua_State* L=luaL_newstate();assert(L);
    luaL_requiref(L,"_G",luaopen_base,1);lua_pop(L,1);
    luaL_requiref(L,"table",luaopen_table,1);lua_pop(L,1);
    luaL_requiref(L,"string",luaopen_string,1);lua_pop(L,1);
    luaL_requiref(L,"math",luaopen_math,1);lua_pop(L,1);
    luaL_requiref(L,"coroutine",luaopen_coroutine,1);lua_pop(L,1);
    if(luaL_dofile(L,"test/automation_runtime_regressions.lua")!=LUA_OK){fprintf(stderr,"scheduler: %s\n",lua_tostring(L,-1));assert(false);}
    lua_close(L);
}
bool regularFile(const std::string& name){struct stat st;return lstat(name.c_str(),&st)==0&&S_ISREG(st.st_mode);}
struct Fixture {
    std::string root,error;
    AutomationWorker* worker;
    Fixture():worker(0){char path[]="/tmp/microhad-automation-unit-XXXXXX";char* made=mkdtemp(path);assert(made);root=made;start();}
    void start(){worker=new AutomationWorker(root,0,0);assert(worker->start(&error));}
    void stop(){delete worker;worker=0;}
    static void clean(const std::string& path){DIR* d=opendir(path.c_str());if(d){dirent* e;while((e=readdir(d))){std::string n=e->d_name;if(n=="."||n=="..")continue;std::string child=path+"/"+n;struct stat st;assert(lstat(child.c_str(),&st)==0);if(S_ISDIR(st.st_mode))clean(child);else assert(unlink(child.c_str())==0);}closedir(d);assert(rmdir(path.c_str())==0);}}
    ~Fixture(){stop();clean(root);}
    void save(const std::string& source,const std::string& name="test.lua"){if(!worker->save(name,source,true,&error)){fprintf(stderr,"save: %s\n",error.c_str());assert(false);}}
    Value status(const std::string& id="test"){Value all;assert(Automation::parse(worker->listJson(),&all));for(size_t i=0;i<all.fields["scripts"].items.size();++i)if(all.fields["scripts"].items[i].fields["id"].string==id)return all.fields["scripts"].items[i];return Value();}
    Value state(const std::string& id="test"){std::string text;assert(readTextFile(root+"/automations/state/"+id+".json",&text));Value v;assert(Automation::parse(text,&v));return v;}
    bool until(const std::string& key,double expected,unsigned milliseconds=2000){for(unsigned i=0;i<milliseconds/10;++i){std::string text;Value v;if(readTextFile(root+"/automations/state/test.json",&text)&&Automation::parse(text,&v)&&v.fields[key].type==Value::Number&&v.fields[key].number==expected)return true;usleep(10000);}return false;}
    void event(const std::string& type,double number=1){Automation::Event e;e.data.fields["type"]=Value(type);e.data.fields["value"]=Value(number);e.data.fields["field"]=Value("temperature_celsius");e.data.fields["device_id"]=Value("sensor");e.data.fields["source"]=Value("zigbee");e.data.fields["chain_id"]=Value("7");e.data.fields["chain_depth"]=Value(0.);assert(worker->enqueue(e));}
};
void serialization(){Value v;assert(Automation::parse("{\"nested\":{\"s\":\"x\\u00e4\\n\",\"n\":-1.2e3},\"b\":false}",&v));Value again;assert(Automation::parse(Automation::json(v),&again)&&again==v);assert(!Automation::parse("{\"n\":01}",&v));assert(!Automation::parse("{\"n\":1e999}",&v));assert(!Automation::parse("{\"x\":1,\"x\":2}",&v));assert(!Automation::parse("{\"x\":true}garbage",&v));Device d;d.ieeeAddress="0xabc";d.name="Office";d.hasTemperature=true;d.temperatureKnown=false;assert(!Automation::snapshot(d).fields.count("temperature_celsius"));d.temperatureKnown=true;d.temperatureCelsius=0;assert(Automation::snapshot(d).fields["temperature_celsius"].number==0);}
void sandboxAndValidation(){Fixture f;assert(f.worker->validate("draft.lua","assert(io==nil and os==nil and package==nil and debug==nil and load==nil and loadfile==nil and dofile==nil and coroutine==nil and rawset==nil); assert(events.on and events.off and automation.sleep and events.wait_for)",&f.error));assert(!f.worker->validate("draft.lua","while true do end",&f.error));assert(f.error.find("budget")!=std::string::npos);assert(!f.worker->validate("draft.lua","local x=string.rep('x',1024*1024)",&f.error));assert(!f.worker->validate("draft.lua","automation.after(0,function()end)",&f.error));assert(!f.worker->validate("draft.lua","events.on({type='unsupported'},function()end)",&f.error));assert(!f.worker->validate("draft.lua","events.on({type='device.value',value_below='cold'},function()end)",&f.error));assert(!f.worker->validate("draft.lua","events.on({type='remote.action',endpoint='one'},function()end)",&f.error));assert(!f.worker->validate("draft.lua","automation.cron('* * *',function()end)",&f.error));assert(!f.worker->validate("draft.lua","automation.cron('70 * * * *',function()end)",&f.error));assert(f.worker->validate("draft.lua","persistent.changed=1; shared.changed=1; automation.after(.01,function()error('must not run')end)",&f.error));assert(!regularFile(f.root+"/automations/state/draft.json"));}
void errorLocations(){
    Fixture f;
    struct Case {const char* code;const char* message;};
    const Case cases[]={
        {"events.on({type='unsupported'},function()end)","unsupported event type"},
        {"events.on({type='device.value',unknown=true},function()end)","unsupported filter field"},
        {"events.on({type='device.value',value_below='cold'},function()end)","must be number"},
        {"automation.after(0,function()end)","invalid duration"},
        {"automation.after(1,'callback')","must be function"},
        {"automation.cron('70 * * * *',function()end)","invalid cron field"},
        {"automation.sleep(1)","sleep is only available in callbacks"},
        {"events.wait_for({type='device.value'},1)","wait_for is only available in callbacks"},
        {"devices.light('missing',{brightness=101})","brightness must be"},
        {"devices.actor('missing','yes')","must be boolean"},
        {"notify.send('title','body',{unknown=true})","invalid notification option"},
        {"notify.send('t','b',{recipients={}})","invalid notification recipients"},
        {"notify.send('t','b',{recipients=''})","invalid notification recipients"},
        {"notify.send('t','b',{recipients={'a',false}})","invalid notification recipients"},
        {"notify.send('t','b',{recipients={[2]='a'}})","invalid notification recipients"},
        {"notify.send('t','b',{recipients={name='a'}})","invalid notification recipients"},
        {"persistent.value=function()end","not serializable"},
        {"local missing=nil; missing()","attempt to call"},
        {"error('custom failure',0)","custom failure"},
        {"assert(false,'assertion failure')","assertion failure"}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
        assert(!f.worker->validate("draft.lua",std::string("-- first line\n\n")+cases[i].code,&f.error));
        if(f.error.find("draft.lua:3: ")!=0){fprintf(stderr,"error location: %s\n",f.error.c_str());assert(false);}
        assert(f.error.find(cases[i].message)!=std::string::npos);
    }
    assert(f.worker->validate("draft.lua","assert(notify.send('t','b',{recipients='Unregistered'}));assert(notify.send('t','b',{recipients={'Alice','Bob'},tag='test'}))",&f.error));
    assert(!f.worker->validate("draft.lua","local function setup()\n  events.on({type='unsupported'},function()end)\nend\nsetup()",&f.error));
    assert(f.error.find("draft.lua:2: unsupported event type")==0);
    assert(!f.worker->validate("draft.lua","-- syntax error\nlocal =",&f.error));
    assert(f.error.find("draft.lua:2:")==0);
    f.save("-- runtime validation failure\n\nevents.on({type='unsupported'},function()end)");
    assert(f.status().fields["error"].string.find("test.lua:3: unsupported event type")==0);
    Value log;assert(Automation::parse(f.worker->logsJson("test.lua"),&log));
    bool found=false;
    for(size_t i=0;i<log.fields["records"].items.size();++i){const Value& row=log.fields["records"].items[i];
        if(row.fields.find("message")->second.string.find("test.lua:3: unsupported event type")==0){assert(row.fields.find("line")->second.number==3);found=true;}}
    assert(found);
    f.save("events.on({type='device.value'},function()\n  automation.sleep(.01)\n  events.wait_for({type='unsupported'},1)\nend)");
    f.event("device.value");
    for(unsigned i=0;i<200&&f.status().fields["error"].string.empty();++i)usleep(10000);
    assert(f.status().fields["error"].string.find("test.lua:3: unsupported event type")==0);
    f.save("log('warn','automation-runtime:34: unrelated message')\nlog('warn','sensor:12: unrelated message')");
    assert(Automation::parse(f.worker->logsJson("test.lua"),&log));
    for(size_t i=0;i<log.fields["records"].items.size();++i){const Value& row=log.fields["records"].items[i];
        if(row.fields.find("message")->second.string.find("unrelated message")!=std::string::npos)assert(!row.fields.count("line"));}
}
void persistenceAndReload(){Fixture f;f.save("persistent.loads=(persistent.loads or 0)+1; persistent.nested={value=2}; persistent.nested.value=3; shared.present=41");assert(f.state().fields["loads"].number==1);assert(f.state().fields["nested"].fields["value"].number==3);assert(f.worker->reload("test.lua",&f.error));assert(f.state().fields["loads"].number==2);f.save("assert(shared.present==41);persistent.loads=(persistent.loads or 0)+1");assert(f.state().fields["loads"].number==3);assert(f.worker->validate("test.lua","persistent.loads=999;shared.present=99",&f.error));assert(f.state().fields["loads"].number==3);f.save("assert(shared.present==41);persistent.loads=999;error('broken init')");assert(!f.status().fields["loaded"].boolean);assert(f.state().fields["loads"].number==3);f.save("persistent.loads=(persistent.loads or 0)+1");assert(f.state().fields["loads"].number==4);assert(f.worker->setEnabled("test.lua",false,&f.error));assert(!f.status().fields["enabled"].boolean);assert(f.state().fields["loads"].number==4);assert(f.worker->setEnabled("test.lua.disabled",true,&f.error));assert(f.state().fields["loads"].number==5);f.stop();f.start();assert(f.worker->reload("test.lua",&f.error));assert(f.state().fields["loads"].number>=6);assert(f.worker->resetPersistent("test.lua",&f.error));assert(f.state().fields.empty());assert(f.worker->reload("test.lua",&f.error));assert(f.state().fields["loads"].number==1);assert(f.worker->remove("test.lua",&f.error));assert(regularFile(f.root+"/automations/state/test.json"));}
void eventsAndCoroutines(){Fixture f;f.save(
  "persistent.count=0;persistent.order=''; "
  "local h=events.on({type='device.value'},function()error('unregistered listener')end);events.off(h);events.off(h);"
  "events.on({type='device.value',source='zigbee',field='temperature_celsius',value_at_least=2,value_below=5},function(e) "
  "assert(not pcall(function()e.value=99 end));persistent.count=persistent.count+1;persistent.order=persistent.order..string.format('%.0f',e.value) end);"
  "events.on({type='device.changed'},function()automation.sleep(.03);local e,why=events.wait_for({type='timer.fired'},.3);assert(e,why);persistent.waited=e.value end)");
  f.event("device.value",1);f.event("device.value",2);f.event("device.value",3);assert(f.until("count",2));assert(f.state().fields["order"].string=="23");f.event("device.changed");usleep(70000);f.event("timer.fired",8);assert(f.until("waited",8));
  f.save("automation.after(.01,function()local e,why=events.wait_for({type='timer.fired'},.02);assert(e==nil and why=='timeout');persistent.timedout=1 end)");assert(f.until("timedout",1));
  f.save("automation.after(.01,function()persistent.failed=99;error('callback rollback')end);automation.after(.03,function()persistent.alive=1 end)");assert(f.until("alive",1));assert(!f.state().fields.count("failed"));
}
void timersLimitsAndLogs(){Fixture f;f.save(
  "local cancelled=automation.after(.01,function()persistent.bad=1 end);assert(automation.cancel(cancelled));assert(not automation.cancel(cancelled));"
  "local h;h=automation.every(.01,function()automation.sleep(.02);persistent.runs=(persistent.runs or 0)+1;if persistent.runs==3 then automation.cancel(h)end end);"
  "automation.after(.005,function()while true do end end);"
  "local t={password='hidden',nested={value=true}};t.self=t;dump(t,'debug table');log('warn','fields',{network_key='hidden',number=3});dump(string.rep('x',10000),'long');log('info',string.rep('\\n',3000),{large=string.rep('x',12000)})");
  assert(f.until("runs",3));usleep(50000);assert(f.state().fields["runs"].number==3);assert(!f.state().fields.count("bad"));std::string logs=f.worker->logsJson("test.lua");assert(logs.find("<cycle>")!=logs.npos);assert(logs.find("[truncated]")!=logs.npos);assert(logs.find("hidden")!=logs.npos);assert(logs.find("budget")!=logs.npos);
  Value records;assert(Automation::parse(logs,&records));for(size_t i=0;i<records.fields["records"].items.size();++i)assert(Automation::json(records.fields["records"].items[i]).size()<=8192);
  assert(!f.worker->validate("draft.lua","local t={};t.self=t;persistent.x=t",&f.error));assert(!f.worker->validate("draft.lua","persistent.x=function()end",&f.error));assert(!f.worker->validate("draft.lua","persistent.x={ [1]=true }",&f.error));assert(!f.worker->validate("draft.lua","persistent.x=0/0",&f.error));assert(!f.worker->validate("draft.lua","for i=1,65 do automation.after(1,function()end) end",&f.error));
  f.save("automation.after(.3,function()persistent.retired=1 end)");assert(f.worker->reload("test.lua",&f.error));f.save("persistent.reloaded=1");usleep(350000);assert(!f.state().fields.count("retired"));
}
void safeFiles(){Fixture f;std::string out;assert(!f.worker->save("../escape.lua","",true,&f.error));assert(!f.worker->save("a/b.lua","",true,&f.error));assert(!f.worker->save("a.lua",std::string(262145,'x'),true,&f.error));std::string symlinkPath=f.root+"/automations/link.lua";assert(symlink("/etc/passwd",symlinkPath.c_str())==0);assert(!f.worker->source("link.lua",&out,&f.error));assert(!f.worker->save("link.lua","x",true,&f.error));assert(!f.worker->remove("link.lua",&f.error));assert(!f.worker->reload("missing.lua",&f.error));f.save("persistent.x=1");assert(!f.worker->save("test.lua.disabled","",false,&f.error));}
void schedulesSharedAndShutdown(){Fixture f;
    FILE* collision=fopen((f.root+"/automations/test.lua").c_str(),"w");assert(collision);fputs("persistent.recovered=1",collision);fclose(collision);
    collision=fopen((f.root+"/automations/test.lua.disabled").c_str(),"w");assert(collision);fputs("-- disabled",collision);fclose(collision);
    assert(f.worker->reload("test.lua",&f.error));assert(!f.status().fields["loaded"].boolean);
    assert(unlink((f.root+"/automations/test.lua.disabled").c_str())==0);assert(f.until("recovered",1));
    f.save("persistent.cron=0;local h=automation.cron('* * * * *',function()persistent.cron=persistent.cron+1 end);local payload={x=1};automation.after(1,function(p)persistent.payload=p.x end,payload);payload.x=2;shared.shared_value=12;events.on({type='automation.error'},function(e)persistent.errors=(persistent.errors or 0)+1 end);automation.after(.01,function()error('expected error event')end)");
    assert(f.until("errors",1));f.worker->advanceTimeForTests(61);assert(f.until("cron",1));assert(f.until("payload",1));f.worker->advanceTimeForTests(3600);assert(f.until("cron",2));
    assert(f.worker->validate("draft.lua","shared.shared_value=99;assert(shared.shared_value==99)",&f.error));
    f.save("assert(shared.shared_value==12);persistent.seen=shared.shared_value;shared.shared_value=13","other.lua");assert(f.state("other").fields["seen"].number==12);
    f.save("assert(shared.shared_value==13);automation.after(.01,function()automation.sleep(100);persistent.stale=1 end)");usleep(50000);assert(f.worker->setEnabled("test.lua",false,&f.error));f.worker->advanceTimeForTests(200);assert(!f.state().fields.count("stale"));
    f.stop();f.start();assert(f.worker->validate("empty.lua","assert(shared.shared_value==nil)",&f.error));assert(f.worker->remove("test.lua.disabled",&f.error));
    f.save("persistent.fresh=1;automation.after(.01,function()events.wait_for({type='timer.fired'});persistent.stale=1 end)");usleep(50000);f.stop();f.start();assert(f.worker->reload("test.lua",&f.error));assert(!f.state().fields.count("stale"));
}
void acceptedReportSources(){Fixture f;Gateway gateway("/dev/null",115200,f.root,"","","",0,f.root+"/history.bin");
    std::vector<unsigned char> value(2,0);value[0]=0x08;value[1]=0x07;
    assert(gateway.applyAttributeReport("0x00124b0000000042",1026,0,value));assert(gateway.applyAttributeReport("0x00124b0000000042",1026,0,value));
    std::vector<Gateway::AutomationReport> reports=gateway.takeAutomationReports();assert(reports.size()==2);assert(reports[0].field=="temperature_celsius");assert(Automation::snapshot(reports[0].device).fields["temperature_celsius"].number==18);
    assert(!gateway.applyAttributeReport("0x00124b0000000042",1026,0,std::vector<unsigned char>(1,0)));assert(gateway.takeAutomationReports().empty());
    assert(gateway.applyIasZoneStatus("0x00124b0000000043",std::vector<unsigned char>(2,0)));assert(gateway.takeAutomationReports().size()==1);assert(!gateway.applyIasZoneStatus("0x00124b0000000043",std::vector<unsigned char>(3,0)));assert(gateway.takeAutomationReports().empty());
    std::string error;assert(gateway.registerShelly("1234567890abcdef","Phone","Phone","fritz",-1,&error));assert(gateway.applyFritzEvent("1234567890abcdef",true,true,65,&error));reports=gateway.takeAutomationReports();assert(reports.size()==2);assert(reports[0].field=="occupied");assert(Automation::snapshot(reports[0].device).fields["occupied"].boolean);
    MatterEndpoint endpoint;endpoint.id="matter:0000000000000001:0000000000000002:0001";endpoint.node="0000000000000002";endpoint.endpoint=1;endpoint.servers.push_back(1026);endpoint.online=true;assert(gateway.reconcileMatterDevices(std::vector<MatterEndpoint>(1,endpoint),&error));MatterReading reading;reading.node=endpoint.node;reading.endpoint=1;reading.cluster=1026;reading.known=true;reading.numberValue=19;assert(gateway.applyMatterReading(reading,&error));assert(gateway.applyMatterReading(reading,&error));reports=gateway.takeAutomationReports();assert(reports.size()==2);assert(Automation::snapshot(reports[0].device).fields["source"].string=="matter");assert(reports[1].field=="temperature_celsius");
    MatterEndpoint remote=endpoint;remote.id="matter:0000000000000001:0000000000000002:0002";remote.endpoint=2;remote.servers.clear();remote.servers.push_back(59);assert(gateway.reconcileMatterDevices(std::vector<MatterEndpoint>(1,remote),&error));MatterButtonEvent press;press.node=remote.node;press.endpoint=remote.endpoint;press.event=0;press.position=1;press.pressCount=2;assert(gateway.applyMatterButtonEvent(press,&error));reports=gateway.takeAutomationReports();assert(reports.size()==1&&reports[0].remoteAction&&reports[0].action=="matter_press"&&reports[0].endpoint==2&&reports[0].position==1&&reports[0].pressCount==2);
}
void callbackBudgetAndQueues(){Fixture f;
    schedulerRegressions();
    f.worker->scanEveryTurnForTests();
    for(unsigned i=1;i<=8;++i){
        std::ostringstream source;source<<"persistent.loads=(persistent.loads or 0)+1 -- version "<<i;
        f.save(source.str());assert(f.state().fields["loads"].number==i);
        assert(f.worker->setEnabled("test.lua",false,&f.error));
        assert(f.worker->setEnabled("test.lua.disabled",true,&f.error));
        assert(f.state().fields["loads"].number==i+1);
        assert(f.worker->resetPersistent("test.lua",&f.error));
        // Reset the expected count for the next distinct replacement.
        if(i<8){std::ostringstream seed;seed<<"persistent.loads="<<i;f.save(seed.str());}
    }
    assert(f.worker->validate("larger.lua","local a=string.rep('x',300000);local b=string.rep('y',300000);assert(#a+#b==600000)",&f.error));
    assert(f.worker->validate("larger.lua","for i=1,128 do events.on({type='device.value'},function()end)end;for i=1,64 do automation.after(1,function()end)end",&f.error));
    assert(!f.worker->validate("larger.lua","for i=1,129 do events.on({type='device.value'},function()end)end",&f.error));
    f.save("automation.after(.01,function()for round=1,4 do local n=0;for i=1,30000 do n=n+i end;automation.sleep(.01)end;persistent.slices=1 end)");
    assert(f.until("slices",1));
    assert(!f.worker->validate("recursive.lua","local function recurse(n)if n==0 then return 0 end return 1+recurse(n-1)end recurse(10000)",&f.error));
    f.save("persistent.good=1;events.on({type='device.value'},function()automation.sleep(.01);automation.sleep(.01);for i=1,40000 do local n=i*i end end);events.on({type='automation.error'},function(e)persistent.limit_errors=(persistent.limit_errors or 0)+1 end)");
    unsigned rejected=0;for(unsigned i=0;i<4096;++i){Automation::Event e;e.data.fields["type"]=Value("device.value");e.data.fields["chain_id"]=Value("");e.data.fields["chain_depth"]=Value(0.);if(!f.worker->enqueue(e))++rejected;}
    assert(rejected>0);assert(f.worker->waitIdle());usleep(100000);assert(f.worker->logsJson("test.lua").find("limit")!=std::string::npos);assert(f.state().fields["good"].number==1);
}
}
void runAutomationUnitTests(){printf("unit: Lua sandbox, validation, persistence, reload, event filtering, coroutines, timers, cron, shared state, limits, logs and all report sources\n");serialization();typedSnapshots();sandboxAndValidation();errorLocations();persistenceAndReload();eventsAndCoroutines();timersLimitsAndLogs();safeFiles();schedulesSharedAndShutdown();acceptedReportSources();callbackBudgetAndQueues();}
