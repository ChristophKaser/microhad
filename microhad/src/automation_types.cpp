#include "automation_types.h"
#include "gateway.h"
#include "json_util.h"
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <cstdio>

namespace Automation {
bool Value::operator==(const Value& v) const {
    return type==v.type && (type==Null || (type==Boolean && boolean==v.boolean) ||
        (type==Number && number==v.number) || (type==String && string==v.string) ||
        (type==Object && fields==v.fields) || (type==Array && items==v.items));
}
std::string json(const Value& v) {
    if(v.type==Value::Null)return "null";
    if(v.type==Value::Boolean)return v.boolean?"true":"false";
    if(v.type==Value::String)return "\""+jsonEscape(v.string)+"\"";
    std::ostringstream out;
    if(v.type==Value::Number){out<<std::setprecision(17)<<v.number;return out.str();}
    out<<(v.type==Value::Object?'{':'[');bool first=true;
    if(v.type==Value::Object)for(std::map<std::string,Value>::const_iterator it=v.fields.begin();it!=v.fields.end();++it){if(!first)out<<',';first=false;out<<'"'<<jsonEscape(it->first)<<"\":"<<json(it->second);}
    else for(unsigned long i=0;i<v.items.size();++i){if(i)out<<',';out<<json(v.items[i]);}
    out<<(v.type==Value::Object?'}':']');return out.str();
}
namespace {
class Parser {
    const std::string& text; size_t at; unsigned long nodes;
    void space(){while(at<text.size()&&(text[at]==' '||text[at]=='\n'||text[at]=='\t'||text[at]=='\r'))++at;}
    bool take(char c){space();if(at==text.size()||text[at]!=c)return false;++at;return true;}
    bool quoted(std::string* out){space();size_t start=at;if(!take('"'))return false;bool escaped=false;while(at<text.size()){char c=text[at++];if(!escaped&&c=='"'){std::string wrapper="{\"s\":"+text.substr(start,at-start)+"}";return jsonStringField(wrapper,"s",out);}if(!escaped&&static_cast<unsigned char>(c)<32)return false;if(!escaped&&c=='\\')escaped=true;else escaped=false;}return false;}
    bool value(Value* out,unsigned int depth){space();if(depth>64||++nodes>16384||at>=text.size())return false;char c=text[at];
        if(c=='"'){out->type=Value::String;return quoted(&out->string);}
        if(c=='{'||c=='['){++at;bool object=c=='{';*out=object?Value::object():Value::array();space();if(at<text.size()&&text[at]==(object?'}':']')){++at;return true;}do{std::string key;if(object&&(!quoted(&key)||out->fields.count(key)||!take(':')))return false;Value child;if(!value(&child,depth+1))return false;if(object)out->fields[key]=child;else out->items.push_back(child);space();if(at<text.size()&&text[at]==(object?'}':']')){++at;return true;}}while(take(','));return false;}
        if(text.compare(at,4,"null")==0){at+=4;*out=Value();return true;}if(text.compare(at,4,"true")==0){at+=4;*out=Value(true);return true;}if(text.compare(at,5,"false")==0){at+=5;*out=Value(false);return true;}
        size_t begin=at;if(c=='-')++at;if(at==text.size())return false;if(text[at]=='0')++at;else{if(text[at]<'1'||text[at]>'9')return false;while(at<text.size()&&text[at]>='0'&&text[at]<='9')++at;}if(at<text.size()&&text[at]=='.'){++at;size_t digits=at;while(at<text.size()&&text[at]>='0'&&text[at]<='9')++at;if(at==digits)return false;}if(at<text.size()&&(text[at]=='e'||text[at]=='E')){++at;if(at<text.size()&&(text[at]=='+'||text[at]=='-'))++at;size_t digits=at;while(at<text.size()&&text[at]>='0'&&text[at]<='9')++at;if(at==digits)return false;}double n=strtod(text.substr(begin,at-begin).c_str(),0);if(!std::isfinite(n))return false;*out=Value(n);return true;
    }
public: explicit Parser(const std::string& t):text(t),at(0),nodes(0){}bool run(Value* v){if(text.size()>524288||!value(v,0))return false;space();return at==text.size();}
};
}
bool parse(const std::string& text,Value* value){return Parser(text).run(value);}
namespace {
std::string folded(const std::string& text) {
    std::string result=text;for(size_t i=0;i<result.size();++i)if(result[i]>='A'&&result[i]<='Z')result[i]+=32;return result;
}
bool dimmable(const Device& d) {
    if(d.hasBrightness)return true;
    for(size_t i=0;i<d.endpoints.size();++i)for(size_t j=0;j<d.endpoints[i].inputClusters.size();++j)
        if(d.endpoints[i].inputClusters[j]==8)return true;
    return false;
}
const char* names[]={"actor_on","air_quality","battery_percent","brightness_percent","co2_ppm","contact_open","humidity_percent","id","kind","last_seen","light_on","link_quality","name","occupied","online","pm25_micrograms_m3","power_watts","source","temperature_celsius","water_detected","wifi_signal_percent","thermostat"};
bool booleanField(Field f) { return f==ActorOn||f==ContactOpen||f==LightOn||f==Occupied||f==Online||f==WaterDetected; }
}
DeviceMetadata::DeviceMetadata(const Device& d):id(d.ieeeAddress),name(d.name),lookupName(folded(d.name)),source(d.source.empty()?"zigbee":d.source),kind(d.kind),matterNode(d.matterNode),matterEndpoint(d.matterEndpoint),light(d.hasLight),actor(d.hasActor),brightness(dimmable(d)),contact(d.hasContact),humidity(d.hasHumidity) {}
bool DeviceMetadata::matches(const Device& d) const {
    return id==d.ieeeAddress&&name==d.name&&source==(d.source.empty()?"zigbee":d.source)&&kind==d.kind&&matterNode==d.matterNode&&matterEndpoint==d.matterEndpoint&&light==d.hasLight&&actor==d.hasActor&&brightness==dimmable(d)&&contact==d.hasContact&&humidity==d.hasHumidity;
}
DeviceSnapshot::DeviceSnapshot():present(0) { for(unsigned i=0;i<FieldCount;++i)values[i]=0; }
const char* fieldName(Field field) { return field<FieldCount?names[field]:""; }
Field findField(const std::string& name) { for(unsigned i=0;i<FieldCount;++i)if(name==names[i])return static_cast<Field>(i);return FieldCount; }
bool sameField(const DeviceSnapshot& a,const DeviceSnapshot& b,Field field) {
    if(a.has(field)!=b.has(field))return false;
    if(!a.has(field))return true;
    switch(field) {
    case Id:return a.metadata->id==b.metadata->id;
    case Name:return a.metadata->name==b.metadata->name;
    case Kind:return a.metadata->kind==b.metadata->kind;
    case Source:return a.metadata->source==b.metadata->source;
    case AirQuality:return a.airQuality==b.airQuality;
    case Thermostat:return a.thermostat==b.thermostat;
    default:return a.values[field]==b.values[field];
    }
}
namespace {
bool sameThermostatInput(const Device& a,const Device& b) {
    const ThermostatState& x=a.thermostat;const ThermostatState& y=b.thermostat;
    return a.source==b.source&&a.modelId==b.modelId&&a.softwareBuildId==b.softwareBuildId&&
        x.values==y.values&&x.unsupported==y.unsupported&&x.attributes==y.attributes&&x.commands==y.commands&&
        x.attributesKnown==y.attributesKnown&&x.commandsKnown==y.commandsKnown&&x.scheduleSupported==y.scheduleSupported&&
        x.schedules==y.schedules&&x.scheduleDetails==y.scheduleDetails&&x.pending==y.pending&&x.sourceId==y.sourceId&&
        x.staleMinutes==y.staleMinutes&&x.sourceObservedAt==y.sourceObservedAt&&x.status==y.status&&x.error==y.error;
}
Value thermostatSnapshotValue(const Device& d) {
    const ThermostatState& t=d.thermostat;Value out=Value::object();
    Value& values=out.fields["values"];values=Value::object();
    for(unsigned i=0;i<ThermostatField::Count;++i){
        const ThermostatField::Id id=static_cast<ThermostatField::Id>(i);
        if(!t.values.count(id))continue;
        const double value=t.values.get(id,0);
        if(!std::isfinite(value))return Value::object();
        values.fields[thermostatAttribute(id)->name]=Value(value);
    }
    Value& supported=out.fields["supported"];supported=Value::object();
    Value& fields=out.fields["fields"];fields=Value::object();
    unsigned long count=0;const ThermostatAttribute* attributes=thermostatAttributes(&count);
    for(unsigned long i=0;i<count;++i){
        const ThermostatAttribute a=thermostatDefinition(d,attributes[i]);
        supported.fields[a.name]=Value(thermostatSupported(d,attributes[i]));
        Value& field=fields.fields[a.name];field=Value::object();
        field.fields["min"]=Value(a.minimum);field.fields["max"]=Value(a.maximum);
        field.fields["step"]=Value(a.step);field.fields["writable"]=Value(a.writable);
    }
    Value& modes=out.fields["modes"];modes=Value::array();
    for(int mode=0;mode<=9;++mode)if(thermostatModeSupported(d,mode))modes.items.push_back(Value(static_cast<double>(mode)));
    const bool trv=isTrvzb(d);
    out.fields["schedule_supported"]=Value(trv||t.scheduleSupported);
    out.fields["schedule_max_transitions"]=Value(static_cast<double>(trv?6:static_cast<int>(::thermostatValue(d,ThermostatField::number_of_daily_transitions,::thermostatValue(d,ThermostatField::number_of_weekly_transitions,10)))));
    out.fields["schedule_midnight_start"]=Value(trv);out.fields["external_input_supported"]=Value(trv);
    Value& details=out.fields["schedule_details"];details=Value::array();
    Value& schedules=out.fields["schedules"];schedules=Value::array();
    for(int day=0;day<7;++day){
        Value detail;std::map<int,std::string>::const_iterator it=t.scheduleDetails.find(day);
        if(it!=t.scheduleDetails.end()&&!parse(it->second,&detail))return Value::object();
        details.items.push_back(detail);it=t.schedules.find(day);
        schedules.items.push_back(it==t.schedules.end()?Value():Value(it->second));
    }
    Value& pending=out.fields["pending"];pending=Value::array();
    for(unsigned i=0;i<ThermostatPendingCount;++i)if(t.pending.count(i))pending.items.push_back(Value(thermostatPendingName(i)));
    out.fields["source_id"]=Value(t.sourceId);out.fields["stale_minutes"]=Value(static_cast<double>(t.staleMinutes));
    out.fields["source_observed_at"]=Value(static_cast<double>(t.sourceObservedAt));
    out.fields["status"]=Value(t.status);out.fields["error"]=Value(t.error);
    return out;
}
}
SnapshotPtr capture(const Device& d,const SnapshotPtr& previous,const DevicePtr& record) {
    DeviceSnapshot next;next.record=record;
    next.metadata=previous&&previous->metadata->matches(d)?previous->metadata:MetadataPtr(new DeviceMetadata(d));
    const bool zigbee=d.source.empty()||d.source=="zigbee";
#define F(field,known,value) if(known){next.present|=1U<<field;next.values[field]=value;}
    F(Id,true,0);F(Name,true,0);F(Kind,true,0);F(Source,true,0);
    F(Online,true,d.online||(!zigbee&&d.source!="matter"&&d.lastSeen>0)||(zigbee&&d.lastSeen>0));
    F(LastSeen,true,d.lastSeen);
    F(Temperature,d.hasTemperature&&d.temperatureKnown,d.temperatureCelsius);
    F(Humidity,d.hasHumidity&&(d.humidityKnown||zigbee),d.humidityPercent);
    F(ContactOpen,d.hasContact&&d.contactKnown,d.contactOpen);
    F(WaterDetected,d.hasWaterLeak&&d.waterLeakKnown,d.waterDetected);
    F(Occupied,d.hasOccupancy,d.occupied);F(LightOn,d.hasLight&&(d.lightKnown||zigbee),d.lightOn);
    F(ActorOn,d.hasActor&&d.actorKnown,d.actorOn);F(Power,d.hasPower&&d.powerKnown,d.powerWatts);
    F(AirQuality,d.hasAirQuality&&d.airQualityKnown,0);if(next.has(AirQuality))next.airQuality=d.airQuality;
    F(Co2,d.hasCo2&&d.co2Known,d.co2Ppm);F(Pm25,d.hasPm25&&d.pm25Known,d.pm25MicrogramsM3);
    F(Brightness,d.hasBrightness&&(d.brightnessKnown||zigbee),d.brightnessPercent);
    F(Battery,d.hasBattery,d.batteryPercent);F(WifiSignal,d.hasWifiSignal,d.wifiSignalPercent);F(LinkQuality,zigbee,d.linkQuality);
    const bool thermostat=isThermostat(d);
    if(thermostat)next.present|=1U<<Thermostat;
    const bool sameThermostat=thermostat&&previous&&previous->has(Thermostat)&&previous->record&&sameThermostatInput(d,*previous->record);
#undef F
    bool same=previous&&(!record||record==previous->record)&&next.metadata==previous->metadata&&next.present==previous->present;
    for(unsigned i=0;same&&i<FieldCount;++i)if(i!=Thermostat)same=sameField(next,*previous,static_cast<Field>(i));
    if(same&&(!thermostat||sameThermostat))return previous;
    if(thermostat){
        next.thermostat=sameThermostat?previous->thermostat:thermostatSnapshotValue(d);
        if(!next.record)next.record=DevicePtr(new Device(d));
    }
    if(same)same=sameField(next,*previous,Thermostat);
    return same?previous:SnapshotPtr(new DeviceSnapshot(next));
}
Value fieldValue(const DeviceSnapshot& d,Field field) {
    if(field>=FieldCount||!d.has(field))return Value();
    switch(field) {
    case Id:return Value(d.metadata->id);case Name:return Value(d.metadata->name);
    case Kind:return Value(d.metadata->kind);case Source:return Value(d.metadata->source);
    case AirQuality:return Value(d.airQuality);
    case Thermostat:return d.thermostat;
    default:return booleanField(field)?Value(d.values[field]!=0):Value(d.values[field]);
    }
}
Value snapshot(const DeviceSnapshot& d) {
    Value result=Value::object();for(unsigned i=0;i<FieldCount;++i)if(d.has(static_cast<Field>(i)))result.fields[names[i]]=fieldValue(d,static_cast<Field>(i));return result;
}
Value snapshot(const Device& d) { return snapshot(*capture(d)); }
Catalog::Catalog():ordered(new SnapshotList),names(new NameIndex),groups(new SceneCatalog) {}
const DeviceSnapshot* Catalog::resolve(const std::string& target,std::string* error) const {
    std::map<std::string,SnapshotPtr>::const_iterator direct=devices.find(target);
    if(direct!=devices.end())return direct->second.get();
    NameIndex::const_iterator name=names->find(folded(target));
    if(name==names->end()){*error="unknown device: "+target;return 0;}
    if(name->second.empty()){*error="ambiguous name; use stable IDs";return 0;}
    direct=devices.find(name->second);return direct==devices.end()?0:direct->second.get();
}
Value eventValue(const Event& e) {
    if(!e.device)return e.data;
    Value out=Value::object();out.fields["type"]=Value(e.type);out.fields["device"]=snapshot(*e.device);
    out.fields["device_id"]=Value(e.device->metadata->id);out.fields["source"]=Value(e.device->metadata->source);
    out.fields["at"]=Value(e.at);char chain[32];snprintf(chain,sizeof(chain),"%llu",static_cast<unsigned long long>(e.chain));
    out.fields["chain_id"]=Value(chain);out.fields["chain_depth"]=Value(static_cast<double>(e.depth));
    if(e.field!=FieldCount){out.fields["field"]=Value(fieldName(e.field));out.fields["value"]=fieldValue(*e.device,e.field);}
    if(e.hasOld)out.fields["old"]=e.old;
    if(e.type=="device.removed")out.fields["reason"]=Value("removed");
    if(e.type=="remote.action"){
        out.fields["action"]=Value(e.action);out.fields["endpoint"]=Value(static_cast<double>(e.endpoint));
        if(e.position>=0)out.fields["position"]=Value(static_cast<double>(e.position));
        if(e.pressCount>=0)out.fields["press_count"]=Value(static_cast<double>(e.pressCount));
    }
    return out;
}

}
