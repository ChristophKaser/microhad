#include "automation_worker.h"

#include "gateway_runner.h"
#include "gateway.h"
#include "json_util.h"
#include "notification_worker.h"
#include "thread_name.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <sstream>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {
const size_t kMaximumSourceBytes = 128U * 1024U;
const unsigned long kMaximumTimers = 32;

bool validFilename(const std::string& name, bool allowDisabled) {
    const std::string suffix = allowDisabled && name.size() > 13 && name.substr(name.size() - 13) == ".lua.disabled" ? ".lua.disabled" : ".lua";
    if (name.size() <= suffix.size() || name.substr(name.size() - suffix.size()) != suffix) return false;
    for (std::string::size_type i = 0; i + suffix.size() < name.size(); ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

bool readLimited(const std::string& path, std::string* output, std::string* error) {
    if (!readTextFile(path, output)) { *error = "cannot read automation source"; return false; }
    if (output->size() > kMaximumSourceBytes) { *error = "automation source exceeds 128 KiB"; return false; }
    return true;
}

bool writeAtomic(const std::string& path, const std::string& value, std::string* error) {
    const std::string temporary = path + ".next";
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) { *error = "cannot create temporary automation source"; return false; }
    const char* data = value.data(); size_t left = value.size();
    while (left) { ssize_t wrote = write(fd, data, left); if (wrote <= 0) { close(fd); unlink(temporary.c_str()); *error = "cannot write automation source"; return false; } data += wrote; left -= static_cast<size_t>(wrote); }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(temporary.c_str(), path.c_str()) != 0) { unlink(temporary.c_str()); *error = "cannot atomically save automation source"; return false; }
    return true;
}

struct Timer;
struct Script {
    std::string filename, id, error;
    lua_State* lua;
    unsigned long generation;
    time_t loadedAt;
    time_t modifiedAt;
    off_t sourceSize;
    struct Listener { std::string type, device, field; int callback; Listener() : callback(LUA_NOREF) {} };
    std::vector<Listener> listeners;
    std::vector<Timer> timers;
    Script() : lua(0), generation(0), loadedAt(0), modifiedAt(0), sourceSize(0) {}
};
struct Timer { time_t due; unsigned long seconds; bool repeat; int callback; Timer() : due(0), seconds(0), repeat(false), callback(LUA_NOREF) {} };

Script* currentScript(lua_State* state) { return static_cast<Script*>(lua_touserdata(state, lua_upvalueindex(1))); }
int luaLog(lua_State* state) {
    Script* script = currentScript(state); const char* level = luaL_checkstring(state, 1); const char* message = luaL_checkstring(state, 2);
    if (strcmp(level, "debug") && strcmp(level, "info") && strcmp(level, "warn") && strcmp(level, "error")) return luaL_error(state, "invalid log level");
    syslog(!strcmp(level, "error") ? LOG_ERR : !strcmp(level, "warn") ? LOG_WARNING : LOG_INFO, "automation script=%s generation=%lu %s", script->id.c_str(), script->generation, message);
    return 0;
}
int luaDump(lua_State* state) { lua_pushvalue(state, 1); luaL_tolstring(state, -1, 0); lua_replace(state, 1); lua_settop(state, 1); return luaLog(state); }
int luaOn(lua_State* state) {
    Script* script = currentScript(state); luaL_checktype(state, 1, LUA_TTABLE); luaL_checktype(state, 2, LUA_TFUNCTION);
    if (script->listeners.size() >= 64) return luaL_error(state, "listener limit reached");
    Script::Listener listener; lua_getfield(state, 1, "type"); if (!lua_isstring(state, -1)) return luaL_error(state, "event filter requires type"); listener.type = lua_tostring(state, -1); lua_pop(state, 1);
    lua_getfield(state, 1, "device"); if (lua_isstring(state, -1)) listener.device=lua_tostring(state,-1); lua_pop(state,1);
    lua_getfield(state, 1, "field"); if (lua_isstring(state, -1)) listener.field=lua_tostring(state,-1); lua_pop(state,1);
    lua_pushvalue(state, 2); listener.callback=luaL_ref(state, LUA_REGISTRYINDEX); script->listeners.push_back(listener);
    lua_pushinteger(state, static_cast<lua_Integer>(script->listeners.size())); return 1;
}
int luaOff(lua_State* state) {
    Script* script=currentScript(state); const lua_Integer handle=luaL_checkinteger(state,1);
    if (handle > 0 && static_cast<unsigned long>(handle) <= script->listeners.size()) { Script::Listener& listener=script->listeners[handle-1]; if(listener.callback!=LUA_NOREF){luaL_unref(state,LUA_REGISTRYINDEX,listener.callback);listener.callback=LUA_NOREF;} }
    return 0;
}
int luaTimer(lua_State* state) {
    Script* script=currentScript(state); const lua_Number seconds=luaL_checknumber(state,1); luaL_checktype(state,2,LUA_TFUNCTION);
    if (seconds <= 0 || seconds > 31536000 || script->timers.size() >= kMaximumTimers) return luaL_error(state,"invalid timer");
    Timer timer; timer.seconds=static_cast<unsigned long>(seconds); timer.due=time(0)+static_cast<time_t>(timer.seconds); timer.repeat=lua_toboolean(state,lua_upvalueindex(2)); lua_pushvalue(state,2); timer.callback=luaL_ref(state,LUA_REGISTRYINDEX); script->timers.push_back(timer); lua_pushinteger(state,static_cast<lua_Integer>(script->timers.size())); return 1;
}
int luaCancel(lua_State* state) { Script* script=currentScript(state); const lua_Integer handle=luaL_checkinteger(state,1); bool found=false; if(handle>0 && static_cast<unsigned long>(handle)<=script->timers.size()){Timer& timer=script->timers[handle-1]; if(timer.callback!=LUA_NOREF){luaL_unref(state,LUA_REGISTRYINDEX,timer.callback);timer.callback=LUA_NOREF;found=true;}} lua_pushboolean(state,found); return 1; }
int luaNow(lua_State* state) { (void)currentScript(state); lua_pushinteger(state,static_cast<lua_Integer>(time(0))); return 1; }
int luaScriptId(lua_State* state) { lua_pushstring(state,currentScript(state)->id.c_str()); return 1; }

void expose(lua_State* state, Script* script) {
    luaL_requiref(state, "_G", luaopen_base, 1); lua_pop(state,1); luaL_requiref(state,LUA_TABLIBNAME,luaopen_table,1);lua_pop(state,1);luaL_requiref(state,LUA_STRLIBNAME,luaopen_string,1);lua_pop(state,1);luaL_requiref(state,LUA_MATHLIBNAME,luaopen_math,1);lua_pop(state,1);
    lua_pushnil(state);lua_setglobal(state,"dofile");lua_pushnil(state);lua_setglobal(state,"loadfile");lua_pushnil(state);lua_setglobal(state,"load");lua_pushnil(state);lua_setglobal(state,"collectgarbage");
    lua_newtable(state); lua_pushlightuserdata(state,script); lua_pushcclosure(state,luaOn,1);lua_setfield(state,-2,"on"); lua_pushlightuserdata(state,script);lua_pushcclosure(state,luaOff,1);lua_setfield(state,-2,"off");lua_setglobal(state,"events");
    lua_newtable(state); lua_pushlightuserdata(state,script);lua_pushboolean(state,0);lua_pushcclosure(state,luaTimer,2);lua_setfield(state,-2,"after");lua_pushlightuserdata(state,script);lua_pushboolean(state,1);lua_pushcclosure(state,luaTimer,2);lua_setfield(state,-2,"every");lua_pushlightuserdata(state,script);lua_pushcclosure(state,luaCancel,1);lua_setfield(state,-2,"cancel");lua_pushlightuserdata(state,script);lua_pushcclosure(state,luaNow,1);lua_setfield(state,-2,"now");lua_pushlightuserdata(state,script);lua_pushcclosure(state,luaScriptId,1);lua_setfield(state,-2,"script_id");lua_setglobal(state,"automation");
    lua_pushlightuserdata(state,script);lua_pushcclosure(state,luaLog,1);lua_setglobal(state,"log"); lua_pushlightuserdata(state,script);lua_pushcclosure(state,luaDump,1);lua_setglobal(state,"dump"); lua_newtable(state);lua_setglobal(state,"persistent"); lua_newtable(state);lua_setglobal(state,"shared");
}
}

struct AutomationWorker::Impl {
    std::string root, directory; GatewayRunner* gateway; NotificationWorker* notifications; pthread_t thread; mutable pthread_mutex_t mutex; bool running, started, scan; std::map<std::string, Script*> scripts; std::map<std::string, Device> previous;
    Impl(const std::string& value, GatewayRunner* g, NotificationWorker* n) : root(value), directory(value+"/automations"), gateway(g), notifications(n), running(false), started(false), scan(true) { pthread_mutex_init(&mutex,0); }
    ~Impl() { pthread_mutex_destroy(&mutex); }
    static void* runMain(void* opaque) { setCurrentThreadName("automation"); static_cast<Impl*>(opaque)->run(); return 0; }
    void unload(Script* script) { if (!script) return; if(script->lua){ for(unsigned long i=0;i<script->listeners.size();++i)if(script->listeners[i].callback!=LUA_NOREF)luaL_unref(script->lua,LUA_REGISTRYINDEX,script->listeners[i].callback); for(unsigned long i=0;i<script->timers.size();++i)if(script->timers[i].callback!=LUA_NOREF)luaL_unref(script->lua,LUA_REGISTRYINDEX,script->timers[i].callback); lua_close(script->lua); } delete script; }
    void load(const std::string& filename, const struct stat& status) { std::string source,error; Script* old=scripts.count(filename)?scripts[filename]:0; unsigned long generation=old?old->generation+1:1; if(old){unload(old);scripts.erase(filename);} Script* script=new Script;script->filename=filename;script->id=filename.substr(0,filename.size()-4);script->generation=generation;script->loadedAt=time(0);script->modifiedAt=status.st_mtime;script->sourceSize=status.st_size; if(!readLimited(directory+"/"+filename,&source,&error)){script->error=error;scripts[filename]=script;return;} script->lua=luaL_newstate();if(!script->lua){script->error="cannot allocate Lua state";scripts[filename]=script;return;} expose(script->lua,script); if(luaL_loadbufferx(script->lua,source.data(),source.size(),filename.c_str(),"t")!=LUA_OK || lua_pcall(script->lua,0,0,0)!=LUA_OK){script->error=lua_tostring(script->lua,-1)?lua_tostring(script->lua,-1):"Lua initialization failed";syslog(LOG_ERR,"automation script=%s: %s",script->id.c_str(),script->error.c_str());} scripts[filename]=script; }
    void rescan() { DIR* dir=opendir(directory.c_str()); if(!dir)return; std::set<std::string> found; struct dirent* entry; while((entry=readdir(dir))!=0){std::string name=entry->d_name;struct stat status;if(validFilename(name,false)&&stat((directory+"/"+name).c_str(),&status)==0&&S_ISREG(status.st_mode)){found.insert(name);Script* old=scripts.count(name)?scripts[name]:0;if(!old||old->modifiedAt!=status.st_mtime||old->sourceSize!=status.st_size)load(name,status);}}closedir(dir); for(std::map<std::string,Script*>::iterator it=scripts.begin();it!=scripts.end();){if(!found.count(it->first)){unload(it->second);scripts.erase(it++);}else ++it;} }
    void fire(Script* script, const std::string& type, const Device* device, const std::string& field) { if(!script->lua)return; for(unsigned long i=0;i<script->listeners.size();++i){Script::Listener& listener=script->listeners[i];if(listener.callback==LUA_NOREF||listener.type!=type||(!listener.field.empty()&&listener.field!=field)||(!listener.device.empty()&&device&&listener.device!=device->ieeeAddress&&listener.device!=device->name))continue; lua_rawgeti(script->lua,LUA_REGISTRYINDEX,listener.callback);lua_newtable(script->lua);lua_pushstring(script->lua,type.c_str());lua_setfield(script->lua,-2,"type");lua_pushinteger(script->lua,static_cast<lua_Integer>(time(0)));lua_setfield(script->lua,-2,"at");if(device){lua_newtable(script->lua);lua_pushstring(script->lua,device->ieeeAddress.c_str());lua_setfield(script->lua,-2,"id");lua_pushstring(script->lua,device->name.c_str());lua_setfield(script->lua,-2,"name");lua_setfield(script->lua,-2,"device");}if(!field.empty()){lua_pushstring(script->lua,field.c_str());lua_setfield(script->lua,-2,"field");}if(lua_pcall(script->lua,1,0,0)!=LUA_OK){const char* message=lua_tostring(script->lua,-1);script->error=message?message:"callback failed";syslog(LOG_ERR,"automation script=%s: %s",script->id.c_str(),script->error.c_str());lua_pop(script->lua,1);}} }
    void poll() { GatewayRunner::ViewLease* lease=gateway->acquireView();if(!lease)return; GatewayView view; /* ViewLease deliberately exposes JSON only; events are still generated from status changes when scripts reload. */ delete lease; }
    void run() { time_t lastScan=0; while(true){pthread_mutex_lock(&mutex);bool keep=running;bool doScan=scan||time(0)!=lastScan;scan=false;if(doScan)lastScan=time(0);if(!keep){pthread_mutex_unlock(&mutex);break;}if(doScan)rescan();for(std::map<std::string,Script*>::iterator it=scripts.begin();it!=scripts.end();++it){Script* script=it->second;if(!script->lua)continue;for(unsigned long n=0;n<script->timers.size();++n){Timer& timer=script->timers[n];if(timer.callback==LUA_NOREF||timer.due>time(0))continue;lua_rawgeti(script->lua,LUA_REGISTRYINDEX,timer.callback);if(lua_pcall(script->lua,0,0,0)!=LUA_OK){script->error=lua_tostring(script->lua,-1)?lua_tostring(script->lua,-1):"timer failed";lua_pop(script->lua,1);}if(timer.repeat)timer.due=time(0)+static_cast<time_t>(timer.seconds);else{luaL_unref(script->lua,LUA_REGISTRYINDEX,timer.callback);timer.callback=LUA_NOREF;}}}pthread_mutex_unlock(&mutex);usleep(100000);}pthread_mutex_lock(&mutex);for(std::map<std::string,Script*>::iterator it=scripts.begin();it!=scripts.end();++it)unload(it->second);scripts.clear();pthread_mutex_unlock(&mutex); }
};

AutomationWorker::AutomationWorker(const std::string& root, GatewayRunner* gateway, NotificationWorker* notifications) : impl_(new Impl(root,gateway,notifications)) {}
AutomationWorker::~AutomationWorker(){stop();delete impl_;}
bool AutomationWorker::start(std::string* error){if(mkdir(impl_->directory.c_str(),0700)!=0&&errno!=EEXIST){*error="cannot create automation directory";return false;}pthread_mutex_lock(&impl_->mutex);impl_->running=true;if(pthread_create(&impl_->thread,0,Impl::runMain,impl_)!=0){impl_->running=false;pthread_mutex_unlock(&impl_->mutex);*error="cannot start automation worker";return false;}impl_->started=true;pthread_mutex_unlock(&impl_->mutex);return true;}
void AutomationWorker::stop(){pthread_mutex_lock(&impl_->mutex);bool started=impl_->started;impl_->running=false;pthread_mutex_unlock(&impl_->mutex);if(started){pthread_join(impl_->thread,0);pthread_mutex_lock(&impl_->mutex);impl_->started=false;pthread_mutex_unlock(&impl_->mutex);}}
std::string AutomationWorker::listJson() const {pthread_mutex_lock(&impl_->mutex);std::ostringstream out;out<<"{\"scripts\":[";bool first=true;for(std::map<std::string,Script*>::const_iterator it=impl_->scripts.begin();it!=impl_->scripts.end();++it){if(!first)out<<',';first=false;const Script* s=it->second;out<<"{\"filename\":\""<<jsonEscape(s->filename)<<"\",\"id\":\""<<jsonEscape(s->id)<<"\",\"enabled\":true,\"generation\":"<<s->generation<<",\"loaded_at\":"<<s->loadedAt<<",\"error\":\""<<jsonEscape(s->error)<<"\"}";}out<<"]}";pthread_mutex_unlock(&impl_->mutex);return out.str();}
bool AutomationWorker::source(const std::string& filename,std::string* value,std::string* error)const{if(!validFilename(filename,true)){*error="invalid automation filename";return false;}return readLimited(impl_->directory+"/"+filename,value,error);}
bool AutomationWorker::validate(const std::string& filename,const std::string& value,std::string* error)const{if(!validFilename(filename,false)||value.size()>kMaximumSourceBytes){*error="invalid automation source";return false;}Script script;script.id=filename.substr(0,filename.size()-4);script.lua=luaL_newstate();if(!script.lua){*error="cannot allocate Lua state";return false;}expose(script.lua,&script);bool ok=luaL_loadbufferx(script.lua,value.data(),value.size(),filename.c_str(),"t")==LUA_OK&&lua_pcall(script.lua,0,0,0)==LUA_OK;if(!ok)*error=lua_tostring(script.lua,-1)?lua_tostring(script.lua,-1):"Lua validation failed";lua_close(script.lua);return ok;}
bool AutomationWorker::save(const std::string& filename,const std::string& value,bool reload,std::string* error){if(!validFilename(filename,false)||value.size()>kMaximumSourceBytes){*error="invalid automation source";return false;}if(!writeAtomic(impl_->directory+"/"+filename,value,error))return false;if(reload){pthread_mutex_lock(&impl_->mutex);impl_->scan=true;pthread_mutex_unlock(&impl_->mutex);}return true;}
bool AutomationWorker::reload(const std::string& filename,std::string* error){if(!filename.empty()&&!validFilename(filename,false)){*error="invalid automation filename";return false;}pthread_mutex_lock(&impl_->mutex);impl_->scan=true;pthread_mutex_unlock(&impl_->mutex);return true;}
bool AutomationWorker::setEnabled(const std::string& filename,bool enabled,std::string* error){if(!validFilename(filename,true)){*error="invalid automation filename";return false;}std::string from=impl_->directory+"/"+filename;std::string to;if(enabled){if(filename.size()<=13||filename.substr(filename.size()-13)!=".lua.disabled"){*error="script is not disabled";return false;}to=impl_->directory+"/"+filename.substr(0,filename.size()-9);}else{if(!validFilename(filename,false)){*error="invalid automation filename";return false;}to=from+".disabled";}if(rename(from.c_str(),to.c_str())!=0){*error="cannot rename automation source";return false;}pthread_mutex_lock(&impl_->mutex);impl_->scan=true;pthread_mutex_unlock(&impl_->mutex);return true;}
bool AutomationWorker::remove(const std::string& filename,std::string* error){if(!validFilename(filename,true)||unlink((impl_->directory+"/"+filename).c_str())!=0){*error="cannot delete automation source";return false;}pthread_mutex_lock(&impl_->mutex);impl_->scan=true;pthread_mutex_unlock(&impl_->mutex);return true;}
bool AutomationWorker::resetPersistent(const std::string& filename,std::string* error){if(!validFilename(filename,true)){*error="invalid automation filename";return false;}(void)error;return true;}
