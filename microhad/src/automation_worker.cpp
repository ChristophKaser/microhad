#include "automation_worker.h"
#include "gateway_runner.h"
#include "gateway.h"
#include "json_util.h"
#include "notification_worker.h"
#include "matter_worker.h"
#include "thread_name.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

using Automation::Value;
namespace {
const char kRuntime[] =
#include "automation_runtime.inc"
;
double monotonic(){timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec+ts.tv_nsec/1e9;}
std::string stem(const std::string& name){size_t end=name.find(".lua");return end==std::string::npos?name:name.substr(0,end);}
bool filename(const std::string& name){std::string id=stem(name);if(id.empty()||id.size()>96||id=="."||id=="..")return false;
    if(name!=id+".lua"&&name!=id+".lua.disabled")return false;
    return id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")==std::string::npos;}
bool disabled(const std::string& name){return name.size()>9&&name.substr(name.size()-9)==".disabled";}
bool regular(const std::string& path){struct stat st;return lstat(path.c_str(),&st)==0&&S_ISREG(st.st_mode);}
bool directory(const std::string& path){struct stat st;return lstat(path.c_str(),&st)==0&&S_ISDIR(st.st_mode);}
bool readSource(const std::string& path,std::string* out,std::string* error,size_t limit=131072){
    int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);struct stat st;
    if(fd<0||fstat(fd,&st)!=0||!S_ISREG(st.st_mode)||st.st_size<0||static_cast<size_t>(st.st_size)>limit){if(fd>=0)close(fd);*error="source is unavailable, unsafe, or exceeds size limit";return false;}
    out->clear();char bytes[4096];ssize_t n;while((n=read(fd,bytes,sizeof(bytes)))!=0){if(n<0){if(errno==EINTR)continue;close(fd);*error="cannot read source";return false;}out->append(bytes,n);if(out->size()>limit){close(fd);*error="source exceeds size limit";return false;}}
    close(fd);return true;
}
bool atomicWrite(const std::string& path,const std::string& content,std::string* error){
    size_t slash=path.rfind('/');std::string parent=path.substr(0,slash);if(!directory(parent)){*error="unsafe automation directory";return false;}
    struct stat st;if(lstat(path.c_str(),&st)==0&&!S_ISREG(st.st_mode)){*error="destination is not a regular file";return false;}
    std::string pattern=parent+"/.automation-XXXXXX";std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);int fd=mkstemp(&name[0]);if(fd<0){*error="cannot create temporary file";return false;}
    bool ok=fchmod(fd,0600)==0;size_t at=0;while(ok&&at<content.size()){ssize_t n=write(fd,content.data()+at,content.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0)ok=false;else at+=n;}
    if(ok)ok=fsync(fd)==0;
    int closed=close(fd);ok=ok&&closed==0;if(ok)ok=rename(&name[0],path.c_str())==0;
    if(ok){int dir=open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);ok=dir>=0&&fsync(dir)==0;if(dir>=0)close(dir);}
    if(!ok){unlink(&name[0]);*error="cannot atomically checkpoint automation file";}return ok;
}
void push(lua_State* L,const Value& v){switch(v.type){case Value::Null:lua_pushnil(L);break;case Value::Boolean:lua_pushboolean(L,v.boolean);break;case Value::Number:lua_pushnumber(L,v.number);break;case Value::String:lua_pushlstring(L,v.string.data(),v.string.size());break;
    case Value::Object:lua_newtable(L);for(std::map<std::string,Value>::const_iterator it=v.fields.begin();it!=v.fields.end();++it){lua_pushlstring(L,it->first.data(),it->first.size());push(L,it->second);lua_rawset(L,-3);}break;
    case Value::Array:lua_newtable(L);for(size_t i=0;i<v.items.size();++i){push(L,v.items[i]);lua_rawseti(L,-2,i+1);}break;}}
bool value(lua_State* L,int index,Value* out,std::set<const void*>* seen,unsigned depth,size_t* bytes,bool arrays=false){
    if(depth>24||*bytes>65536)return false;
    index=lua_absindex(L,index);int t=lua_type(L,index);*bytes+=8;
    if(t==LUA_TNIL){*out=Value();return true;}if(t==LUA_TBOOLEAN){*out=Value(lua_toboolean(L,index)!=0);return true;}
    if(t==LUA_TNUMBER){double n=lua_tonumber(L,index);if(!std::isfinite(n))return false;*out=Value(n);return true;}
    if(t==LUA_TSTRING){size_t n=0;const char* s=lua_tolstring(L,index,&n);*bytes+=n;if(*bytes>65536)return false;*out=Value(std::string(s,n));return true;}
    if(t!=LUA_TTABLE||lua_getmetatable(L,index)){if(t==LUA_TTABLE)lua_pop(L,1);return false;}
    const void* id=lua_topointer(L,index);if(seen->count(id))return false;seen->insert(id);*out=Value::object();bool ok=true;lua_pushnil(L);
    while(lua_next(L,index)){Value child;if(!value(L,-1,&child,seen,depth+1,bytes,arrays)){lua_pop(L,2);ok=false;break;}if(lua_type(L,-2)==LUA_TSTRING){size_t n;const char* key=lua_tolstring(L,-2,&n);*bytes+=n;out->fields[std::string(key,n)]=child;}
        else if(arrays&&lua_isinteger(L,-2)&&lua_tointeger(L,-2)>0&&lua_tointeger(L,-2)<=8192){size_t n=lua_tointeger(L,-2);if(out->items.size()<n)out->items.resize(n);out->items[n-1]=child;}
        else{lua_pop(L,2);ok=false;break;}lua_pop(L,1);if(*bytes>65536){lua_pop(L,1);ok=false;break;}}
    seen->erase(id);if(!out->items.empty()){if(!out->fields.empty())ok=false;out->type=Value::Array;}return ok;
}
bool readValue(lua_State* L,int index,Value* out,bool arrays=false){std::set<const void*> seen;size_t bytes=0;return value(L,index,out,&seen,0,&bytes,arrays);}
std::string lower(std::string s){for(size_t i=0;i<s.size();++i)if(s[i]>='A'&&s[i]<='Z')s[i]+=32;return s;}
bool secret(const std::string& key){std::string s=lower(key);return s.find("password")!=s.npos||s.find("credential")!=s.npos||s.find("token")!=s.npos||s.find("secret")!=s.npos||s.find("private_key")!=s.npos||s.find("network_key")!=s.npos||s.find("identity")!=s.npos;}
Value redact(Value v){for(std::map<std::string,Value>::iterator it=v.fields.begin();it!=v.fields.end();++it)it->second=secret(it->first)?Value("[redacted]"):redact(it->second);for(size_t i=0;i<v.items.size();++i)v.items[i]=redact(v.items[i]);return v;}
std::string stringArg(lua_State* L,int i){luaL_checktype(L,i,LUA_TSTRING);size_t n=0;const char* s=lua_tolstring(L,i,&n);return std::string(s,n);}
int failure(lua_State* L,const std::string& message){lua_pushnil(L);lua_pushlstring(L,message.data(),message.size());return 2;}
}

struct AutomationWorker::Impl {
    struct Script;
    struct Request {std::string operation,name,source,error;bool done,result;Request():done(false),result(false){}};
    struct Transaction {Value persistent,base;Transaction():persistent(Value::object()),base(Value::object()){} };
    struct Script {
        Impl* owner;std::string name,id,error,source,transaction;lua_State* L;int runtime;size_t heap;bool validation,failed,enabled;unsigned long generation,instructions;double deadline,began,remaining;time_t loaded,validated,lastEvent,lastCallback;unsigned listeners,timers,suspended;std::map<std::string,Transaction> transactions;Value persistent,validationShared;std::string chain;unsigned depth;std::deque<Value> errors;
        Script(Impl* o):owner(o),L(0),runtime(LUA_NOREF),heap(0),validation(false),failed(false),enabled(true),generation(0),instructions(0),deadline(0),began(0),remaining(0),loaded(0),validated(0),lastEvent(0),lastCallback(0),listeners(0),timers(0),suspended(0),persistent(Value::object()),depth(0){}
        ~Script(){if(L)lua_close(L);}
    };
    AutomationWorker* api;std::string root,dir;GatewayRunner* gateway;NotificationWorker* notifications;MatterWorker* matter;
    mutable pthread_mutex_t mutex;pthread_cond_t ready;pthread_t thread;bool running,started,busy;std::deque<Request*> requests;std::deque<Automation::Event> events;std::map<std::string,Script*> scripts;Value shared;std::map<std::string,Value> statuses;std::deque<Value> logs;unsigned long nextLog,dropped;std::vector<Device> devices;std::vector<SceneGroup> groups;double testOffset;
    Impl(AutomationWorker* a,const std::string& r,GatewayRunner* g,NotificationWorker* n):api(a),root(r),dir(r+"/automations"),gateway(g),notifications(n),matter(0),running(false),started(false),busy(false),shared(Value::object()),nextLog(0),dropped(0),testOffset(0){pthread_mutex_init(&mutex,0);pthread_cond_init(&ready,0);}
    ~Impl(){pthread_cond_destroy(&ready);pthread_mutex_destroy(&mutex);}
    static Script* script(lua_State* L){return static_cast<Script*>(lua_touserdata(L,lua_upvalueindex(1)));}
    static void* allocate(void* opaque,void* old,size_t oldSize,size_t newSize){Script* s=static_cast<Script*>(opaque);if(!old)oldSize=0;if(!newSize){free(old);s->heap-=oldSize;return 0;}if(newSize>524288||s->heap-oldSize>524288-newSize)return 0;void* memory=realloc(old,newSize);if(memory)s->heap=s->heap-oldSize+newSize;return memory;}
    static void hook(lua_State* L,lua_Debug*){void* opaque=0;lua_getallocf(L,&opaque);Script* s=static_cast<Script*>(opaque);s->instructions+=100;lua_Debug frame;if(lua_getstack(L,64,&frame))luaL_error(L,"callback recursion limit exceeded");if(s->instructions>=static_cast<unsigned long>(s->remaining)||monotonic()>=s->deadline)luaL_error(L,"callback execution budget exceeded");}
    void record(Script* s,const std::string& level,const std::string& message,const Value& fields=Value::object(),const std::string& operation="log"){
        Value row=Value::object();row.fields["script_id"]=Value(s->id);row.fields["generation"]=Value(static_cast<double>(s->generation));row.fields["at"]=Value(static_cast<double>(time(0)));row.fields["level"]=Value(level);row.fields["operation"]=Value(operation);row.fields["message"]=Value(message.size()>4096?message.substr(0,4060)+" [truncated]":message);row.fields["fields"]=redact(fields);
        size_t colon=message.find(':');if(colon!=message.npos){char* end=0;long line=strtol(message.c_str()+colon+1,&end,10);if(line>0&&end&&*end==':')row.fields["line"]=Value(static_cast<double>(line));}
        // Bound the encoded record, not just its message: structured fields and
        // JSON escaping must not let one author consume the entire log ring.
        if(Automation::json(row).size()>4000){
            row.fields["fields"]=Value::object();
            row.fields["fields"].fields["truncated"]=Value(true);
            std::string& text=row.fields["message"].string;
            while(Automation::json(row).size()>3980&&!text.empty())text.resize(text.size()/2);
            text+=" [truncated]";
        }
        pthread_mutex_lock(&mutex);row.fields["record_id"]=Value(static_cast<double>(++nextLog));logs.push_back(row);if(logs.size()>256)logs.pop_front();pthread_mutex_unlock(&mutex);
        if(!s->validation)syslog(level=="error"?LOG_ERR:level=="warn"?LOG_WARNING:level=="debug"?LOG_DEBUG:LOG_INFO,"automation script=%s generation=%lu %s",s->id.c_str(),s->generation,row.fields["message"].string.c_str());
    }
    void error(Script* s,const std::string& message,const std::string& operation){s->error=message;s->failed=true;record(s,"error",message,Value::object(),operation);if(!s->validation&&s->chain!="automation.error"&&s->errors.size()<16){Value event=Value::object();event.fields["type"]=Value("automation.error");event.fields["at"]=Value(static_cast<double>(time(0)));event.fields["source"]=Value("automation");event.fields["operation"]=Value(operation);event.fields["message"]=Value(message);event.fields["chain_id"]=Value("automation.error");event.fields["chain_depth"]=Value(0.);s->errors.push_back(event);}}
    static int now(lua_State* L){lua_pushnumber(L,time(0)+script(L)->owner->testOffset);return 1;}
    static int clock(lua_State* L){lua_pushnumber(L,monotonic()+script(L)->owner->testOffset);return 1;}
    static int id(lua_State* L){lua_pushstring(L,script(L)->id.c_str());return 1;}
    static int calendar(lua_State* L){time_t epoch=time(0)+static_cast<time_t>(script(L)->owner->testOffset);tm local;localtime_r(&epoch,&local);lua_newtable(L);int values[]={local.tm_min,local.tm_hour,local.tm_mday,local.tm_mon+1,local.tm_wday,local.tm_year,local.tm_yday};for(int i=0;i<7;++i){lua_pushinteger(L,values[i]);lua_rawseti(L,-2,i+1);}return 1;}
    static int handle(lua_State* L){unsigned long* h=static_cast<unsigned long*>(lua_newuserdatauv(L,sizeof(unsigned long),0));*h=static_cast<unsigned long>(luaL_checkinteger(L,1));return 1;}
    static int handleId(lua_State* L){luaL_checktype(L,1,LUA_TUSERDATA);lua_pushinteger(L,*static_cast<unsigned long*>(lua_touserdata(L,1)));return 1;}
    static int limit(lua_State* L){Script* s=script(L);lua_State* target=lua_tothread(L,1);if(!target)return luaL_error(L,"expected coroutine");s->remaining=luaL_checknumber(L,2);s->instructions=0;s->began=monotonic();s->deadline=s->began+luaL_checknumber(L,3);lua_sethook(target,hook,LUA_MASKCOUNT,100);return 0;}
    static int spent(lua_State* L){Script* s=script(L);lua_pushnumber(L,s->instructions);lua_pushnumber(L,monotonic()-s->began);return 2;}
    static int begin(lua_State* L){Script* s=script(L);s->transaction=stringArg(L,1);s->chain=stringArg(L,2);s->depth=static_cast<unsigned>(luaL_checkinteger(L,3));if(!s->transactions.count(s->transaction)){s->transactions[s->transaction].persistent=s->persistent;s->transactions[s->transaction].base=s->persistent;}return 0;}
    static Value merge(const Value& base,const Value& staged,Value current){if(base.type!=Value::Object||staged.type!=Value::Object||current.type!=Value::Object)return staged;for(std::map<std::string,Value>::const_iterator it=base.fields.begin();it!=base.fields.end();++it)if(!staged.fields.count(it->first))current.fields.erase(it->first);for(std::map<std::string,Value>::const_iterator it=staged.fields.begin();it!=staged.fields.end();++it){std::map<std::string,Value>::const_iterator old=base.fields.find(it->first);if(old==base.fields.end())current.fields[it->first]=it->second;else if(old->second!=it->second)current.fields[it->first]=merge(old->second,it->second,current.fields[it->first]);}return current;}
    static int finish(lua_State* L){Script* s=script(L);bool ok=lua_toboolean(L,1)!=0;bool done=lua_toboolean(L,2)!=0;if(done){if(ok&&!s->validation){Transaction& tx=s->transactions[s->transaction];Value next=merge(tx.base,tx.persistent,s->persistent);std::string err;if(Automation::json(next).size()>65536)s->owner->error(s,"persistent state exceeds 64 KiB","persistent");else if(next!=s->persistent&&!atomicWrite(s->owner->dir+"/state/"+s->id+".json",Automation::json(next),&err))s->owner->error(s,err,"persistent");else s->persistent=next;}s->transactions.erase(s->transaction);s->lastCallback=time(0);}s->transaction.clear();return 0;}
    static int discard(lua_State* L){script(L)->transactions.erase(stringArg(L,1));return 0;}
    static int readState(lua_State* L){Script* s=script(L);std::string scope=stringArg(L,1);push(L,scope=="shared"?(s->validation?s->validationShared:s->owner->shared):s->transactions.count(s->transaction)?s->transactions[s->transaction].persistent:s->persistent);return 1;}
    static int writeState(lua_State* L){Script* s=script(L);std::string scope=stringArg(L,1);Value v;bool ok=readValue(L,2,&v)&&v.type==Value::Object&&Automation::json(v).size()<=65536;if(!ok)return luaL_error(L,"state must contain finite, acyclic values with string keys (64 KiB limit)");if(scope=="shared"){if(s->validation)s->validationShared=v;else s->owner->shared=v;}else s->transactions[s->transaction].persistent=v;return 0;}
    static int serializable(lua_State* L){Value v;bool ok=readValue(L,1,&v);if(!ok)return luaL_error(L,"value is not serializable");push(L,v);return 1;}
    static int failed(lua_State* L){lua_pushboolean(L,script(L)->failed);return 1;}
    static int failureLog(lua_State* L){Script* s=script(L);s->owner->error(s,stringArg(L,1),stringArg(L,2));return 0;}
    static int log(lua_State* L){Script* s=script(L);std::string level=stringArg(L,1),message=stringArg(L,2);if(level!="debug"&&level!="info"&&level!="warn"&&level!="error")return luaL_error(L,"invalid severity");Value fields=Value::object();if(!lua_isnoneornil(L,3)&&!readValue(L,3,&fields,true))return luaL_error(L,"log fields must be JSON compatible");if(!s->validation)s->owner->record(s,level,message,fields);return 0;}
    static void dumpValue(lua_State* L,int at,std::string* out,std::set<const void*>* seen,unsigned depth){if(out->size()>4096)return;at=lua_absindex(L,at);if(lua_type(L,at)==LUA_TTABLE){const void* p=lua_topointer(L,at);if(seen->count(p)){*out+="<cycle>";return;}if(depth>24){*out+="<depth limit>";return;}seen->insert(p);*out+="{";lua_pushnil(L);while(lua_next(L,at)){if(lua_type(L,-2)==LUA_TSTRING&&secret(lua_tostring(L,-2))){*out+=lua_tostring(L,-2);*out+="=[redacted],";}else{dumpValue(L,-2,out,seen,depth+1);*out+="=";dumpValue(L,-1,out,seen,depth+1);*out+=",";}lua_pop(L,1);if(out->size()>4096){lua_pop(L,1);break;}}*out+="}";seen->erase(p);}else if(lua_type(L,at)==LUA_TSTRING){size_t n;const char* text=lua_tolstring(L,at,&n);out->append(text,std::min(n,static_cast<size_t>(4100)));}else if(lua_type(L,at)==LUA_TNUMBER){std::ostringstream n;n<<lua_tonumber(L,at);*out+=n.str();}else if(lua_type(L,at)==LUA_TBOOLEAN)*out+=lua_toboolean(L,at)?"true":"false";else{std::ostringstream p;p<<lua_typename(L,lua_type(L,at))<<':'<<lua_topointer(L,at);*out+=p.str();}}
    static int dump(lua_State* L){Script* s=script(L);std::string text=lua_isnoneornil(L,2)?"":stringArg(L,2)+": ";std::set<const void*> seen;dumpValue(L,1,&text,&seen,0);if(!s->validation)s->owner->record(s,"debug",text);return 0;}
    const Device* resolve(const std::string& target,std::string* err){const Device* result=0;for(size_t i=0;i<devices.size();++i){Value snap=Automation::snapshot(devices[i]);if(target==snap.fields["id"].string||target==devices[i].ieeeAddress)return &devices[i];if(lower(target)==lower(devices[i].name)){if(result){*err="ambiguous name; use stable IDs "+Automation::snapshot(*result).fields["id"].string+" or "+snap.fields["id"].string;return 0;}result=&devices[i];}}if(!result)*err="unknown device: "+target;return result;}
    static int resolveId(lua_State* L){Script* s=script(L);std::string err;const Device* d=s->owner->resolve(stringArg(L,1),&err);if(!d)return failure(L,err);push(L,Automation::snapshot(*d).fields["id"]);return 1;}
    static int deviceGet(lua_State* L){Script* s=script(L);std::string err;const Device* d=s->owner->resolve(stringArg(L,1),&err);if(!d)return failure(L,err);push(L,Automation::snapshot(*d));return 1;}
    static int deviceList(lua_State* L){Script* s=script(L);Value f=Value::object();if(!lua_isnoneornil(L,1)&&(!readValue(L,1,&f)||f.type!=Value::Object))return luaL_error(L,"invalid device filter");for(std::map<std::string,Value>::const_iterator it=f.fields.begin();it!=f.fields.end();++it)if((it->first!="source"&&it->first!="kind"&&it->first!="online"&&it->first!="name")||(it->first=="online"?it->second.type!=Value::Boolean:it->second.type!=Value::String))return luaL_error(L,"invalid device filter field");Value list=Value::array();for(size_t i=0;i<s->owner->devices.size();++i){Value v=Automation::snapshot(s->owner->devices[i]);bool match=true;for(std::map<std::string,Value>::const_iterator it=f.fields.begin();it!=f.fields.end();++it)if(v.fields[it->first]!=it->second)match=false;if(match)list.items.push_back(v);}push(L,list);return 1;}
    static int action(lua_State* L){Script* s=script(L);Automation::Action a;a.operation=stringArg(L,1);std::string err;const Device* d=s->owner->resolve(stringArg(L,2),&err);if(!readValue(L,3,&a.args))return luaL_error(L,"invalid action arguments");if(!d)return failure(L,err);
        bool dimmable=d->hasBrightness;
        for(size_t endpoint=0;endpoint<d->endpoints.size();++endpoint)
            for(size_t cluster=0;cluster<d->endpoints[endpoint].inputClusters.size();++cluster)
                if(d->endpoints[endpoint].inputClusters[cluster]==8)dimmable=true;
        if((a.operation=="actor"&&!d->hasActor)||(a.operation=="light"&&(!d->hasLight||(a.args.fields.count("brightness")&&!dimmable))))return failure(L,"unsupported device capability");
        if(s->validation){lua_pushboolean(L,1);return 1;}a.target=d->ieeeAddress;a.script=s->id;a.generation=s->generation;a.chain=strtoull(s->chain.c_str(),0,10);a.depth=s->depth+1;if(a.depth>16)return failure(L,"causal chain limit reached");
        if(d->source=="matter"){
            if(!s->owner->matter)return failure(L,"Matter worker unavailable");
            std::ostringstream request;request<<"{\"schema\":1,\"node\":\""<<d->matterNode<<"\",\"endpoint\":"<<d->matterEndpoint;
            unsigned char opcode=11;if(a.operation=="refresh"){opcode=10;request<<",\"cluster\":"<<(d->hasLight||d->hasActor?6:d->hasContact?69:d->hasHumidity?1029:1026)<<",\"attribute\":0";}else if(a.args.fields.count("brightness")){request<<",\"command\":\"move_to_level\",\"level\":"<<static_cast<int>((a.args.fields["brightness"].number*254+50)/100)<<",\"transition_deciseconds\":0";}else request<<",\"command\":\""<<(a.args.fields["on"].boolean?"on":"off")<<"\"";request<<"}";uint32_t id;std::string prepared;bool ok=opcode==10?MatterChannel::deviceReadRequest(request.str(),&prepared):MatterChannel::deviceCommandRequest(request.str(),&prepared);if(!ok||!s->owner->matter->submit(opcode,prepared,&id,&err))return failure(L,err.empty()?"unsupported Matter operation":err);
            if(a.args.fields.count("brightness")&&a.args.fields.count("on")){
                std::ostringstream power;
                power<<"{\"schema\":1,\"node\":\""<<d->matterNode<<"\",\"endpoint\":"<<d->matterEndpoint
                     <<",\"command\":\""<<(a.args.fields["on"].boolean?"on":"off")<<"\"}";
                if(!MatterChannel::deviceCommandRequest(power.str(),&prepared)||!s->owner->matter->submit(11,prepared,&id,&err))
                    return failure(L,err.empty()?"unsupported Matter power operation":err);
            }
        }else if(!s->owner->gateway||!s->owner->gateway->submitAutomation(a,&err))return failure(L,err.empty()?"coordinator unavailable":err);
        lua_pushboolean(L,1);return 1;
    }
    static bool targetArg(lua_State* L,int index,std::string* target){if(lua_type(L,index)==LUA_TSTRING){*target=stringArg(L,index);return true;}return false;}
    static int scene(lua_State* L){Script* s=script(L);std::string groupName,sceneName;if(!targetArg(L,1,&groupName)||(!lua_isnil(L,2)&&!targetArg(L,2,&sceneName)))return luaL_error(L,"scene targets must be names or string IDs");const SceneGroup* group=0;for(size_t i=0;i<s->owner->groups.size();++i){const SceneGroup& g=s->owner->groups[i];std::ostringstream id;id<<g.id;if(id.str()==groupName){group=&g;break;}if(lower(g.name)==lower(groupName)){if(group)return failure(L,"ambiguous scene group; use stable ID");group=&g;}}if(!group)return failure(L,"unknown scene group");int selected=-1;if(!lua_isnil(L,2)){for(size_t i=0;i<group->scenes.size();++i){std::ostringstream id;id<<static_cast<int>(group->scenes[i].id);if(id.str()==sceneName){selected=i;break;}if(lower(group->scenes[i].name)==lower(sceneName)){if(selected>=0)return failure(L,"ambiguous scene; use stable ID");selected=i;}}if(selected<0)return failure(L,"unknown scene");}
        Automation::Action a;a.operation=selected<0?"scene.off":"scene.activate";a.args.fields["group"]=Value(static_cast<double>(group->id));if(selected>=0)a.args.fields["scene"]=Value(static_cast<double>(group->scenes[selected].id));a.script=s->id;a.generation=s->generation;a.chain=strtoull(s->chain.c_str(),0,10);a.depth=s->depth+1;std::string err;if(!s->validation&&(!s->owner->gateway||!s->owner->gateway->submitAutomation(a,&err)))return failure(L,err);lua_pushboolean(L,1);return 1;}
    static int notify(lua_State* L){
        Script* s=script(L);
        std::string title=stringArg(L,1),body=stringArg(L,2);
        Value options=Value::object();
        if(title.size()>256||body.size()>4096)return luaL_error(L,"notification exceeds size limit");
        if(!lua_isnoneornil(L,3)&&(!readValue(L,3,&options)||options.type!=Value::Object))
            return luaL_error(L,"invalid notification options");
        for(std::map<std::string,Value>::const_iterator it=options.fields.begin();it!=options.fields.end();++it)
            if((it->first!="device_id"&&it->first!="tag")||it->second.type!=Value::String||it->second.string.size()>128)
                return luaL_error(L,"invalid notification option");
        if(s->validation){lua_pushboolean(L,1);return 1;}
        std::string err;
        NotificationWorker* notifications=s->owner->notifications;
        bool accepted=false;
        if(notifications){
            const std::string device=options.fields["device_id"].string,tag=options.fields["tag"].string;
            if(device.empty()&&tag.empty())accepted=notifications->enqueue(title,body,&err);
            else{
                // Keep author grouping separate from Android's channel setting.
                std::ostringstream key;
                key<<"automation:"<<s->id<<":"<<device.size()<<":"<<device<<":"<<tag;
                accepted=notifications->enqueueState(title,body,key.str(),"show",std::vector<std::string>(),&err);
            }
        }
        if(!accepted)return failure(L,err.empty()?"notifications unavailable":err);
        lua_pushboolean(L,1);return 1;
    }
    void bind(Script* s,const char* name,lua_CFunction fn){lua_pushlightuserdata(s->L,s);lua_pushcclosure(s->L,fn,1);lua_setfield(s->L,-2,name);}
    bool create(Script* s){s->L=lua_newstate(allocate,s);if(!s->L){error(s,"cannot allocate Lua state","load");return false;}lua_State* L=s->L;
        luaL_requiref(L,"_G",luaopen_base,1);lua_pop(L,1);luaL_requiref(L,"table",luaopen_table,1);lua_pop(L,1);luaL_requiref(L,"string",luaopen_string,1);lua_pop(L,1);luaL_requiref(L,"math",luaopen_math,1);lua_pop(L,1);luaL_requiref(L,"coroutine",luaopen_coroutine,1);lua_pop(L,1);
        if(luaL_loadbufferx(L,kRuntime,sizeof(kRuntime)-1,"@automation-runtime","t")!=LUA_OK){error(s,lua_tostring(L,-1),"runtime");return false;}
        lua_newtable(L);
#define B(name,fn) bind(s,name,fn)
        B("now",now);B("monotonic",clock);B("script_id",id);B("calendar",calendar);B("handle",handle);B("handle_id",handleId);B("limit",limit);B("spent",spent);B("begin",begin);B("finish",finish);B("discard",discard);B("read_state",readState);B("write_state",writeState);B("serializable",serializable);B("failed",failed);B("failure",failureLog);B("log",log);B("dump",dump);B("resolve",resolveId);B("device_get",deviceGet);B("device_list",deviceList);B("action",action);B("scene",scene);B("notify",notify);
#undef B
        if(lua_pcall(L,1,1,0)!=LUA_OK){error(s,lua_tostring(L,-1),"runtime");return false;}s->runtime=luaL_ref(L,LUA_REGISTRYINDEX);return true;
    }
    struct Invocation {Script* s;const char* method;const Value* argument;int results;};
    static int invoke(lua_State* L){Invocation* i=static_cast<Invocation*>(lua_touserdata(L,1));lua_rawgeti(L,LUA_REGISTRYINDEX,i->s->runtime);lua_getfield(L,-1,i->method);if(i->argument)push(L,*i->argument);lua_call(L,i->argument?1:0,i->results);return i->results;}
    bool call(Script* s,const char* method,const Value* argument=0){if(!s->L)return false;lua_State* L=s->L;int base=lua_gettop(L);Invocation i={s,method,argument,0};lua_pushcfunction(L,invoke);lua_pushlightuserdata(L,&i);int rc=lua_pcall(L,1,0,0);if(rc!=LUA_OK){const char* msg=lua_tostring(L,-1);error(s,msg?msg:"runtime error",method);}lua_settop(L,base);return rc==LUA_OK;}
    bool initialize(Script* s,const std::string& source){if(!create(s))return false;lua_State* L=s->L;lua_rawgeti(L,LUA_REGISTRYINDEX,s->runtime);lua_getfield(L,-1,"initialize");if(luaL_loadbufferx(L,source.data(),source.size(),("@"+s->name).c_str(),"t")!=LUA_OK){error(s,lua_tostring(L,-1),"syntax");lua_settop(L,0);return false;}lua_rawgeti(L,LUA_REGISTRYINDEX,s->runtime);lua_getfield(L,-1,"env");lua_remove(L,-2);lua_setupvalue(L,-2,1);bool ok=lua_pcall(L,1,1,0)==LUA_OK;if(!ok)error(s,lua_tostring(L,-1),"initialization");else ok=lua_toboolean(L,-1)!=0;lua_settop(L,0);return ok;}
    void status(Script* s){Value v=Value::object();if(s->L&&s->enabled){lua_State* L=s->L;Invocation invocation={s,"stats",0,3};lua_pushcfunction(L,invoke);lua_pushlightuserdata(L,&invocation);if(lua_pcall(L,1,3,0)==LUA_OK){s->listeners=lua_tointeger(L,-3);s->timers=lua_tointeger(L,-2);s->suspended=lua_tointeger(L,-1);}lua_settop(L,0);}v.fields["filename"]=Value(s->name);v.fields["id"]=Value(s->id);v.fields["enabled"]=Value(s->enabled);v.fields["loaded"]=Value(s->L!=0);v.fields["generation"]=Value(static_cast<double>(s->generation));v.fields["loaded_at"]=Value(static_cast<double>(s->loaded));v.fields["validated_at"]=Value(static_cast<double>(s->validated));v.fields["last_event_at"]=Value(static_cast<double>(s->lastEvent));v.fields["last_callback_at"]=Value(static_cast<double>(s->lastCallback));v.fields["error"]=Value(s->error);v.fields["listeners"]=Value(static_cast<double>(s->listeners));v.fields["timers"]=Value(static_cast<double>(s->timers));v.fields["coroutines"]=Value(static_cast<double>(s->suspended));v.fields["heap_bytes"]=Value(static_cast<double>(s->heap));pthread_mutex_lock(&mutex);statuses[s->id]=v;pthread_mutex_unlock(&mutex);}
    void catalog(){if(!gateway)return;GatewayRunner::ViewLease* lease=gateway->acquireView();if(lease){devices=lease->devices();groups=lease->sceneGroups();delete lease;}}
    void scan(const std::string& force=""){
        DIR* entries=opendir(dir.c_str());if(!entries)return;std::map<std::string,std::string> found;std::set<std::string> collisions;dirent* entry;
        while((entry=readdir(entries))!=0){std::string name=entry->d_name;if(filename(name)&&regular(dir+"/"+name)){std::string id=stem(name);if(found.count(id))collisions.insert(id);else found[id]=name;}}closedir(entries);
        for(std::map<std::string,Script*>::iterator it=scripts.begin();it!=scripts.end();){if(!found.count(it->first)){if(gateway)gateway->automationGeneration(it->first,0);delete it->second;pthread_mutex_lock(&mutex);statuses.erase(it->first);pthread_mutex_unlock(&mutex);scripts.erase(it++);}else ++it;}
        for(std::map<std::string,std::string>::iterator it=found.begin();it!=found.end();++it){std::string source,err;bool readable=readSource(dir+"/"+it->second,&source,&err);Script* old=scripts.count(it->first)?scripts[it->first]:0;bool conflict=collisions.count(it->first)!=0;
            if(old&&old->name==it->second&&old->source==source&&force!="*"&&force!=it->first&&old->enabled==(!disabled(it->second)&&!conflict))continue;
            unsigned long generation=old?old->generation+1:1;time_t validated=old?old->validated:0;if(gateway)gateway->automationGeneration(it->first,0);delete old;
            Script* s=new Script(this);s->name=it->second;s->id=it->first;s->generation=generation;s->validated=validated;s->source=source;s->enabled=!disabled(s->name)&&!conflict;s->loaded=time(0);scripts[s->id]=s;
            if(conflict)error(s,"both enabled and disabled source exist for this ID","load");else if(!readable)error(s,err,"load");else if(s->enabled){std::string state;if(regular(dir+"/state/"+s->id+".json")){if(!readSource(dir+"/state/"+s->id+".json",&state,&err,65536)||!Automation::parse(state,&s->persistent)||s->persistent.type!=Value::Object){error(s,"invalid persistent checkpoint; reset explicitly to recover","persistent");status(s);continue;}}if(gateway)gateway->automationGeneration(s->id,s->generation);if(!initialize(s,source)){if(gateway)gateway->automationGeneration(s->id,0);if(s->L)lua_close(s->L);s->L=0;s->listeners=s->timers=s->suspended=0;}else record(s,"info","script loaded",Value::object(),"load");}status(s);
        }
    }
    void process(Request* r){r->result=true;if(r->operation=="validate"){Script candidate(this);candidate.name=r->name;candidate.id=stem(r->name);candidate.validation=true;candidate.validationShared=shared;std::map<std::string,Script*>::iterator live=scripts.find(candidate.id);if(live!=scripts.end())candidate.persistent=live->second->persistent;r->result=initialize(&candidate,r->source);r->error=candidate.error;if(r->result&&live!=scripts.end()){live->second->validated=time(0);status(live->second);}}
        else if(r->operation=="advance"){testOffset+=strtod(r->source.c_str(),0);}
        else if(r->operation=="reset"){r->result=atomicWrite(dir+"/state/"+stem(r->name)+".json","{}",&r->error);if(r->result){std::map<std::string,Script*>::iterator live=scripts.find(stem(r->name));if(live!=scripts.end()){live->second->persistent=Value::object();for(std::map<std::string,Transaction>::iterator it=live->second->transactions.begin();it!=live->second->transactions.end();++it){it->second.base=Value::object();it->second.persistent=Value::object();}}}}
        else scan(r->name.empty()?"*":stem(r->name));
    }
    static void* main(void* opaque){setCurrentThreadName("automation");static_cast<Impl*>(opaque)->run();return 0;}
    void run(){double nextScan=0;while(true){std::deque<Request*> pending;std::deque<Automation::Event> queued;unsigned long overflow;
            pthread_mutex_lock(&mutex);bool keep=running;pending.swap(requests);queued.swap(events);overflow=dropped;dropped=0;busy=true;pthread_mutex_unlock(&mutex);if(!keep){for(size_t i=0;i<pending.size();++i){pending[i]->error="automation worker stopping";pending[i]->result=false;pthread_mutex_lock(&mutex);pending[i]->done=true;pthread_cond_broadcast(&ready);pthread_mutex_unlock(&mutex);}break;}
            catalog();if(monotonic()>=nextScan){scan();nextScan=monotonic()+0.25;}for(size_t i=0;i<pending.size();++i){process(pending[i]);pthread_mutex_lock(&mutex);pending[i]->done=true;pthread_cond_broadcast(&ready);pthread_mutex_unlock(&mutex);}
            for(size_t i=0;i<queued.size();++i)for(std::map<std::string,Script*>::iterator it=scripts.begin();it!=scripts.end();++it){Script* s=it->second;if(s->L){s->lastEvent=time(0);call(s,"event",&queued[i].data);}}
            for(std::map<std::string,Script*>::iterator it=scripts.begin();it!=scripts.end();++it){Script* s=it->second;if(s->L){if(overflow)error(s,"event queue limit reached; reports dropped","queue");std::deque<Value> errors;errors.swap(s->errors);for(size_t i=0;i<errors.size();++i)call(s,"event",&errors[i]);call(s,"tick");}status(s);}
            pthread_mutex_lock(&mutex);busy=false;pthread_cond_broadcast(&ready);timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=10000000;if(until.tv_nsec>=1000000000){++until.tv_sec;until.tv_nsec-=1000000000;}if(requests.empty()&&events.empty()&&running)pthread_cond_timedwait(&ready,&mutex,&until);pthread_mutex_unlock(&mutex);
        }for(std::map<std::string,Script*>::iterator it=scripts.begin();it!=scripts.end();++it){if(gateway)gateway->automationGeneration(it->first,0);delete it->second;}scripts.clear();}
    bool request(Request* r){pthread_mutex_lock(&mutex);if(!running||requests.size()>=32){pthread_mutex_unlock(&mutex);r->error="automation worker unavailable or queue full";return false;}requests.push_back(r);pthread_cond_broadcast(&ready);while(!r->done)pthread_cond_wait(&ready,&mutex);pthread_mutex_unlock(&mutex);return r->result;}
};

AutomationWorker::AutomationWorker(const std::string& root,GatewayRunner* gateway,NotificationWorker* n):impl_(new Impl(this,root,gateway,n)){}
AutomationWorker::~AutomationWorker(){stop();delete impl_;}
void AutomationWorker::setMatterWorker(MatterWorker* matter){impl_->matter=matter;}
bool AutomationWorker::start(std::string* err){if((mkdir(impl_->dir.c_str(),0700)!=0&&errno!=EEXIST)||!directory(impl_->dir)){*err="unsafe automation directory";return false;}std::string state=impl_->dir+"/state";if((mkdir(state.c_str(),0700)!=0&&errno!=EEXIST)||!directory(state)){*err="unsafe automation state directory";return false;}pthread_mutex_lock(&impl_->mutex);if(impl_->started){pthread_mutex_unlock(&impl_->mutex);*err="automation already started";return false;}impl_->running=true;if(pthread_create(&impl_->thread,0,Impl::main,impl_)!=0){impl_->running=false;pthread_mutex_unlock(&impl_->mutex);*err="cannot start automation worker";return false;}impl_->started=true;pthread_mutex_unlock(&impl_->mutex);if(impl_->gateway)impl_->gateway->setAutomationWorker(this);return true;}
void AutomationWorker::stop(){if(impl_->gateway)impl_->gateway->setAutomationWorker(0);pthread_mutex_lock(&impl_->mutex);bool started=impl_->started;impl_->running=false;pthread_cond_broadcast(&impl_->ready);pthread_mutex_unlock(&impl_->mutex);if(started)pthread_join(impl_->thread,0);pthread_mutex_lock(&impl_->mutex);impl_->started=false;pthread_mutex_unlock(&impl_->mutex);}
std::string AutomationWorker::listJson()const{Value out=Value::object();out.fields["scripts"]=Value::array();pthread_mutex_lock(&impl_->mutex);for(std::map<std::string,Value>::const_iterator it=impl_->statuses.begin();it!=impl_->statuses.end();++it)out.fields["scripts"].items.push_back(it->second);pthread_mutex_unlock(&impl_->mutex);return Automation::json(out);}
std::string AutomationWorker::logsJson(const std::string& name)const{Value out=Value::object();out.fields["records"]=Value::array();pthread_mutex_lock(&impl_->mutex);for(std::deque<Value>::const_iterator it=impl_->logs.begin();it!=impl_->logs.end();++it)if(name.empty()||it->fields.find("script_id")->second.string==stem(name))out.fields["records"].items.push_back(*it);pthread_mutex_unlock(&impl_->mutex);return Automation::json(out);}
bool AutomationWorker::source(const std::string& name,std::string* out,std::string* err)const{if(!filename(name)){*err="invalid automation filename";return false;}return readSource(impl_->dir+"/"+name,out,err);}
bool AutomationWorker::validate(const std::string& name,const std::string& source,std::string* err)const{if(!filename(name)||source.size()>131072){*err="invalid automation source";return false;}Impl::Request r;r.operation="validate";r.name=name;r.source=source;bool ok=impl_->request(&r);*err=r.error;return ok;}
bool AutomationWorker::save(const std::string& name,const std::string& source,bool reload,std::string* err){if(!filename(name)||source.size()>131072){*err="invalid automation source";return false;}std::string other=impl_->dir+"/"+stem(name)+(disabled(name)?".lua":".lua.disabled");if(regular(other)){*err="another enabled/disabled source already owns this ID";return false;}if(!atomicWrite(impl_->dir+"/"+name,source,err))return false;if(reload)return this->reload(name,err);return true;}
bool AutomationWorker::reload(const std::string& name,std::string* err){if(!name.empty()&&!filename(name)){*err="invalid automation filename";return false;}if(!name.empty()&&!regular(impl_->dir+"/"+name)){*err="source does not exist";return false;}Impl::Request r;r.operation="reload";r.name=name;bool ok=impl_->request(&r);*err=r.error;return ok;}
bool AutomationWorker::setEnabled(const std::string& name,bool enabled,std::string* err){if(!filename(name)||!regular(impl_->dir+"/"+name)){*err="invalid source";return false;}if(enabled==!disabled(name))return true;std::string target=stem(name)+(enabled?".lua":".lua.disabled");struct stat st;if(lstat((impl_->dir+"/"+target).c_str(),&st)==0||rename((impl_->dir+"/"+name).c_str(),(impl_->dir+"/"+target).c_str())!=0){*err="cannot rename automation source without overwriting a file";return false;}return reload(target,err);}
bool AutomationWorker::remove(const std::string& name,std::string* err){if(!filename(name)||!regular(impl_->dir+"/"+name)||unlink((impl_->dir+"/"+name).c_str())!=0){*err="cannot delete source";return false;}Impl::Request r;r.operation="reload";r.name=name;bool ok=impl_->request(&r);*err=r.error;return ok;}
bool AutomationWorker::resetPersistent(const std::string& name,std::string* err){if(!filename(name)){*err="invalid automation filename";return false;}Impl::Request r;r.operation="reset";r.name=name;bool ok=impl_->request(&r);*err=r.error;return ok;}
bool AutomationWorker::enqueue(const Automation::Event& event){pthread_mutex_lock(&impl_->mutex);if(!impl_->running){pthread_mutex_unlock(&impl_->mutex);return false;}if(impl_->events.size()>=256){++impl_->dropped;pthread_mutex_unlock(&impl_->mutex);return false;}impl_->events.push_back(event);pthread_cond_signal(&impl_->ready);pthread_mutex_unlock(&impl_->mutex);return true;}
bool AutomationWorker::waitIdle(unsigned long ms)const{double until=monotonic()+ms/1000.;while(monotonic()<until){pthread_mutex_lock(&impl_->mutex);bool idle=!impl_->busy&&impl_->events.empty()&&impl_->requests.empty();pthread_mutex_unlock(&impl_->mutex);if(idle)return true;usleep(1000);}return false;}
#ifdef MICROHAD_TESTING
void AutomationWorker::advanceTimeForTests(double seconds){Impl::Request r;r.operation="advance";std::ostringstream value;value<<seconds;r.source=value.str();impl_->request(&r);}
#endif
