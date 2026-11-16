#include "automation_types.h"
#include "gateway.h"
#include "json_util.h"
#include "automation_profile.h"
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
bool schedule(const Value& value, MatterProto::Measurement* result) {
    if(value.type!=Value::Object || value.fields.size()!=2 || !value.fields.count("mode") || !value.fields.count("transitions")) return false;
    const Value& mode=value.fields.find("mode")->second; const Value& rows=value.fields.find("transitions")->second;
    if(mode.type!=Value::Number || mode.number<1 || mode.number>3 || mode.number!=std::floor(mode.number) || rows.type!=Value::Array || rows.items.empty() || rows.items.size()>32) return false;
    MatterProto::Measurement out; out.mode=static_cast<unsigned>(mode.number); out.transitions.present=true;
    for(size_t i=0;i<rows.items.size();++i) {
        const Value& row=rows.items[i]; if(row.type!=Value::Object || !row.fields.count("time") || row.fields.size()!=1+((out.mode.value&1)?1U:0U)+((out.mode.value&2)?1U:0U)) return false;
        const Value& time=row.fields.find("time")->second; if(time.type!=Value::Number || !std::isfinite(time.number) || time.number<0 || time.number>=1440 || time.number!=std::floor(time.number)) return false;
        MatterProto::ThermostatTransition t; t.time=static_cast<uint32_t>(time.number);
        for(unsigned bit=1;bit<=2;bit<<=1) if(out.mode.value&bit) {
            std::map<std::string,Value>::const_iterator it=row.fields.find(bit==1?"heat":"cool");
            if(it==row.fields.end() || it->second.type!=Value::Number || !std::isfinite(it->second.number)) return false;
            if(bit==1)t.heat=it->second.number;else t.cool=it->second.number;
        }
        out.transitions.value.items.push_back(t);
    }
    *result=out;return true;
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
size_t escapedSize(const std::string& text) {
    size_t size=2;
    for(size_t i=0;i<text.size();++i){unsigned char c=static_cast<unsigned char>(text[i]);size+=(c=='\\'||c=='"'||c=='\b'||c=='\f'||c=='\n'||c=='\r'||c=='\t')?2:c<32?6:1;}
    return size;
}
}
size_t jsonSize(const Value& v) {
    if(v.type==Value::Null)return 4;
    if(v.type==Value::Boolean)return v.boolean?4:5;
    if(v.type==Value::String)return escapedSize(v.string);
    if(v.type==Value::Number){std::ostringstream out;out<<std::setprecision(17)<<v.number;return out.str().size();}
    size_t size=2;
    if(v.type==Value::Object){for(std::map<std::string,Value>::const_iterator i=v.fields.begin();i!=v.fields.end();++i){if(i!=v.fields.begin())++size;size+=escapedSize(i->first)+1+jsonSize(i->second);}}
    else for(size_t i=0;i<v.items.size();++i){if(i)++size;size+=jsonSize(v.items[i]);}
    return size;
}
namespace {
class Parser {
    const std::string& text; size_t at; unsigned long nodes;
    void space(){while(at<text.size()&&(text[at]==' '||text[at]=='\n'||text[at]=='\t'||text[at]=='\r'))++at;}
    bool take(char c){space();if(at==text.size()||text[at]!=c)return false;++at;return true;}
    bool quoted(std::string* out){space();size_t start=at;if(!take('"'))return false;bool escaped=false;while(at<text.size()){char c=text[at++];if(!escaped&&c=='"')return jsonStringAt(text,start,at,out);if(!escaped&&static_cast<unsigned char>(c)<32)return false;if(!escaped&&c=='\\')escaped=true;else escaped=false;}return false;}
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
    if(d.brightness.present())return true;
    for(size_t i=0;i<d.zigbee.read().endpoints.size();++i)for(size_t j=0;j<d.zigbee.read().endpoints[i].inputClusters.size();++j)
        if(d.zigbee.read().endpoints[i].inputClusters[j]==8)return true;
    return false;
}
const char* names[]={"actor_on","air_quality","battery_percent","brightness_percent","co2_ppm","contact_open","humidity_percent","id","kind","last_seen","light_on","link_quality","name","occupied","online","pm25_micrograms_m3","power_watts","source","temperature_celsius","water_detected","wifi_signal_percent","thermostat","electrical",
#define ELECTRICAL(c,a,s,k,u,d,m) "electrical." #k,
#include "../../config/electrical-measurements.def"
#undef ELECTRICAL
};
bool booleanField(Field f) { return f==ActorOn||f==ContactOpen||f==LightOn||f==Occupied||f==Online||f==WaterDetected; }
}
DeviceMetadata::DeviceMetadata(const Device& d):id(d.ieeeAddress),name(d.name),lookupName(folded(d.name)),source(d.source.empty()?"zigbee":d.source),kind(d.kind),matterNode(d.matter.read().node),matterEndpoint(d.matter.read().endpoint),light(d.light.present()),actor(d.actor.present()),brightness(dimmable(d)),contact(d.contact.present()),humidity(d.humidity.present()) {}
bool DeviceMetadata::matches(const Device& d) const {
    return id==d.ieeeAddress&&name==d.name&&source==(d.source.empty()?"zigbee":d.source)&&kind==d.kind&&matterNode==d.matter.read().node&&matterEndpoint==d.matter.read().endpoint&&light==d.light.present()&&actor==d.actor.present()&&brightness==dimmable(d)&&contact==d.contact.present()&&humidity==d.humidity.present();
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
    case Electrical:return a.electrical==b.electrical||(a.electrical&&b.electrical&&*a.electrical==*b.electrical);
    case Thermostat:return a.thermostat==b.thermostat||(a.thermostat&&b.thermostat&&*a.thermostat==*b.thermostat);
    default:return a.values[field]==b.values[field];
    }
}
namespace {
bool sameThermostatCapabilities(const Device& a,const Device& b) {
    if(isTrvzb(a)!=isTrvzb(b))return false;
    const ThermostatState& x=a.thermostat.read();const ThermostatState& y=b.thermostat.read();
    if(x.scheduleSupported!=y.scheduleSupported)return false;
    unsigned long count;const ThermostatAttribute* fields=thermostatAttributes(&count);
    for(unsigned long i=0;i<count;++i){
        const ThermostatAttribute left=thermostatDefinition(a,fields[i]),right=thermostatDefinition(b,fields[i]);
        if(left.minimum!=right.minimum||left.maximum!=right.maximum||left.step!=right.step||left.writable!=right.writable||
           thermostatSupported(a,fields[i])!=thermostatSupported(b,fields[i]))return false;
    }
    for(int mode=0;mode<=9;++mode)if(thermostatModeSupported(a,mode)!=thermostatModeSupported(b,mode))return false;
    return ::thermostatValue(a,ThermostatField::number_of_daily_transitions,::thermostatValue(a,ThermostatField::number_of_weekly_transitions,10))==
           ::thermostatValue(b,ThermostatField::number_of_daily_transitions,::thermostatValue(b,ThermostatField::number_of_weekly_transitions,10));
}
bool sameElectricalInput(const ElectricalState& a,const ElectricalState& b) {
    if(a.resetMetadata!=b.resetMetadata||a.values.size()!=b.values.size())return false;
    std::map<std::string,ElectricalMeasurement>::const_iterator x=a.values.begin(),y=b.values.begin();
    for(;x!=a.values.end();++x,++y){
        const ElectricalMeasurement& l=x->second;const ElectricalMeasurement& r=y->second;
        if(x->first!=y->first||l.known!=r.known||l.value!=r.value||l.unit!=r.unit||l.metadata!=r.metadata||l.observedAt!=r.observedAt||l.endpoint!=r.endpoint)return false;
    }
    return true;
}
Value electricalSnapshotValue(const ElectricalState& state) {
    Value result=Value::object();
    for(std::map<std::string,ElectricalMeasurement>::const_iterator i=state.values.begin();i!=state.values.end();++i){
        if(!electricalDefinition(i->first))continue;
        const ElectricalMeasurement& v=i->second;
        Value entry=Value::object();
        for(std::map<std::string,uint64_t>::const_iterator m=v.metadata.begin();m!=v.metadata.end();++m)entry.fields[m->first]=Value(static_cast<double>(m->second));
        entry.fields["value"]=v.known?Value(v.value):Value();
        entry.fields["unit"]=Value(v.unit);
        entry.fields["quality"]=Value(v.known?"measured":"unknown");
        entry.fields["observed_at"]=Value(static_cast<double>(v.observedAt));
        entry.fields["endpoint"]=Value(static_cast<double>(v.endpoint));
        result.fields[i->first]=entry;
    }
    if(!state.resetMetadata.empty()){
        Value reset=Value::object();
        for(std::map<std::string,uint64_t>::const_iterator m=state.resetMetadata.begin();m!=state.resetMetadata.end();++m)reset.fields[m->first]=Value(static_cast<double>(m->second));
        result.fields["cumulative_energy_reset"]=reset;
    }
    return result;
}
Value thermostatSnapshotPart(const Device& d,unsigned part) {
    const ThermostatState& t=d.thermostat.read();Value out=Value::object();
    if(part==0) {
    Value& values=out.fields["values"];values=Value::object();
    for(unsigned i=0;i<ThermostatField::Count;++i){
        const ThermostatField::Id id=static_cast<ThermostatField::Id>(i);
        if(!t.values.count(id))continue;
        const double value=t.values.get(id,0);
        if(!std::isfinite(value))return Value::object();
        values.fields[thermostatAttribute(id)->name]=Value(value);
    }
    } else if(part==1) {
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
    } else if(part==2) {
    Value& details=out.fields["schedule_details"];details=Value::array();
    Value& schedules=out.fields["schedules"];schedules=Value::array();
    Value& sources=out.fields["schedule_sources"];sources=Value::array();
    for(int day=0;day<7;++day){
        Value detail;std::map<int,std::vector<unsigned char> >::const_iterator it=t.scheduleDetails.find(day);
        if(it!=t.scheduleDetails.end()&&it->second.size()>=3&&it->second[2]>=1&&it->second[2]<=3&&it->second.size()==3UL+it->second[0]*(2+((it->second[2]&1)?2:0)+((it->second[2]&2)?2:0))){
            const std::vector<unsigned char>& p=it->second;detail=Value::object();detail.fields["mode"]=Value(static_cast<double>(p[2]));
            Value& transitions=detail.fields["transitions"];transitions=Value::array();
            unsigned width=2+((p[2]&1)?2:0)+((p[2]&2)?2:0);
            for(unsigned i=0;i<p[0];++i){unsigned at=3+i*width;Value row=Value::object();row.fields["time"]=Value(static_cast<double>(p[at]|(p[at+1]<<8)));at+=2;
                if(p[2]&1){row.fields["heat"]=Value(static_cast<short>(p[at]|(p[at+1]<<8))/100.0);at+=2;}
                if(p[2]&2)row.fields["cool"]=Value(static_cast<short>(p[at]|(p[at+1]<<8))/100.0);
                transitions.items.push_back(row);
            }
        }
        details.items.push_back(detail);std::map<int,std::string>::const_iterator text=t.schedules.find(day);
        schedules.items.push_back(text!=t.schedules.end()?Value(text->second):detail);
        text=t.scheduleSources.find(day);sources.items.push_back(text==t.scheduleSources.end()?Value():Value(text->second));
    }
    } else {
    Value& pending=out.fields["pending"];pending=Value::array();
    for(unsigned i=0;i<ThermostatPendingCount;++i)if(t.pending.count(i))pending.items.push_back(Value(thermostatPendingName(i)));
    out.fields["source_id"]=Value(d.thermostatSourceId);out.fields["stale_minutes"]=Value(static_cast<double>(d.thermostatStaleMinutes));
    out.fields["source_observed_at"]=Value(static_cast<double>(t.sourceObservedAt));
    out.fields["status"]=Value(t.status);out.fields["error"]=Value(t.error);
    }
    return out;
}
ValuePtr thermostatPart(const Device& d,unsigned part) {
    AUTOMATION_PROFILE("snapshot.thermostat.part");
    return ValuePtr(new Value(thermostatSnapshotPart(d,part)));
}
ThermostatSnapshotPtr captureThermostat(const Device& d,const SnapshotPtr& previous) {
    const ThermostatSnapshot* old=previous&&previous->thermostat&&previous->record?previous->thermostat.get():0;
    const Device* before=old?previous->record.get():0;
    const ThermostatState& t=d.thermostat.read();
    ThermostatSnapshot* next=new ThermostatSnapshot;
    ThermostatSnapshotPtr result(next);
    next->readings=old&&t.values==before->thermostat.read().values?old->readings:thermostatPart(d,0);
    next->capabilities=old&&sameThermostatCapabilities(d,*before)?old->capabilities:thermostatPart(d,1);
    next->schedules=old&&t.schedules==before->thermostat.read().schedules&&t.scheduleDetails==before->thermostat.read().scheduleDetails&&t.scheduleSources==before->thermostat.read().scheduleSources?old->schedules:thermostatPart(d,2);
    next->status=old&&t.pending==before->thermostat.read().pending&&t.status==before->thermostat.read().status&&t.error==before->thermostat.read().error&&t.sourceObservedAt==before->thermostat.read().sourceObservedAt&&d.thermostatSourceId==before->thermostatSourceId&&d.thermostatStaleMinutes==before->thermostatStaleMinutes?old->status:thermostatPart(d,3);
    if(old&&next->readings==old->readings&&next->capabilities==old->capabilities&&next->schedules==old->schedules&&next->status==old->status)return previous->thermostat;
    return result;
}
}
Value ThermostatSnapshot::materialize() const {
    Value result=Value::object();const ValuePtr parts[]={readings,capabilities,schedules,status};
    for(unsigned i=0;i<4;++i)if(parts[i])result.fields.insert(parts[i]->fields.begin(),parts[i]->fields.end());
    return result;
}
bool ThermostatSnapshot::operator==(const ThermostatSnapshot& other) const {
    const ValuePtr left[]={readings,capabilities,schedules,status},right[]={other.readings,other.capabilities,other.schedules,other.status};
    for(unsigned i=0;i<4;++i)if(left[i]!=right[i]&&(!left[i]||!right[i]||*left[i]!=*right[i]))return false;
    return true;
}
SnapshotPtr capture(const Device& d,const SnapshotPtr& previous,const DevicePtr& record) {
    DeviceSnapshot next;next.record=record;
    next.metadata=previous&&previous->metadata->matches(d)?previous->metadata:MetadataPtr(new DeviceMetadata(d));
    const bool zigbee=d.source.empty()||d.source=="zigbee";
#define F(field,known,value) if(known){next.present|=1ULL<<field;next.values[field]=value;}
    F(Id,true,0);F(Name,true,0);F(Kind,true,0);F(Source,true,0);
    F(Online,true,d.online||(!zigbee&&d.source!="matter"&&d.lastSeen>0)||(zigbee&&d.lastSeen>0));
    F(LastSeen,true,d.lastSeen);
    F(Temperature,d.temperature.present()&&d.temperature.read().known,d.temperature.read().celsius);
    F(Humidity,d.humidity.present()&&(d.humidity.read().known||zigbee),d.humidity.read().percent);
    F(ContactOpen,d.contact.present()&&d.contact.read().known,d.contact.read().open);
    F(WaterDetected,d.waterLeak.present()&&d.waterLeak.read().known,d.waterLeak.read().detected);
    F(Occupied,d.occupancy.present(),d.occupancy.read().occupied);F(LightOn,d.light.present()&&(d.light.read().known||zigbee),d.light.read().on);
    F(ActorOn,d.actor.present()&&d.actor.read().known,d.actor.read().on);F(Power,d.power.present()&&d.power.read().known,d.power.read().watts);
    F(AirQuality,d.airQuality.present()&&d.airQuality.read().known,0);if(next.has(AirQuality))next.airQuality=d.airQuality.read().value;
    F(Co2,d.co2.present()&&d.co2.read().known,d.co2.read().ppm);F(Pm25,d.pm25.present()&&d.pm25.read().known,d.pm25.read().microgramsM3);
    F(Brightness,d.brightness.present()&&(d.brightness.read().known||zigbee),d.brightness.read().percent);
    F(Battery,d.battery.present(),d.battery.read().percent);F(WifiSignal,d.wifiSignal.present(),d.wifiSignal.read().percent);F(LinkQuality,zigbee,d.zigbee.read().linkQuality);
    if(d.electrical.present()) {
        next.present|=1ULL<<Electrical;
        next.electrical=previous&&previous->electrical&&previous->record&&previous->record->electrical.present()&&sameElectricalInput(d.electrical.read(),previous->record->electrical.read())?previous->electrical:ValuePtr(new Value(electricalSnapshotValue(d.electrical.read())));
    }
    if(d.electrical.present()) {
#define ELECTRICAL(c,a,s,k,u,div,m) {std::map<std::string,ElectricalMeasurement>::const_iterator e=d.electrical.read().values.find(#k);if(e!=d.electrical.read().values.end()) {F(Electrical_##k,e->second.known,e->second.value);}}
#include "../../config/electrical-measurements.def"
#undef ELECTRICAL
    }
    if(d.electrical.present()&&!next.record)next.record=DevicePtr(new Device(d));
    const bool thermostat=isThermostat(d);
    if(thermostat)next.present|=1ULL<<Thermostat;
#undef F
    if(thermostat){
        AUTOMATION_PROFILE("snapshot.thermostat.build");
        next.thermostat=captureThermostat(d,previous);
        if(!next.record)next.record=DevicePtr(new Device(d));
    }
    bool same=previous&&(!record||record==previous->record)&&next.metadata==previous->metadata&&next.present==previous->present;
    for(unsigned i=0;same&&i<FieldCount;++i)same=sameField(next,*previous,static_cast<Field>(i));
    if(same)return previous;
    { AUTOMATION_PROFILE("snapshot.allocate"); return SnapshotPtr(new DeviceSnapshot(next)); }
}
Value fieldValue(const DeviceSnapshot& d,Field field) {
    if(field>=FieldCount||!d.has(field))return Value();
    switch(field) {
    case Id:return Value(d.metadata->id);case Name:return Value(d.metadata->name);
    case Kind:return Value(d.metadata->kind);case Source:return Value(d.metadata->source);
    case AirQuality:return Value(d.airQuality);
    case Electrical:return d.electrical?*d.electrical:Value();
    case Thermostat:return d.thermostat?d.thermostat->materialize():Value();
    default:return booleanField(field)?Value(d.values[field]!=0):Value(d.values[field]);
    }
}
Value snapshot(const DeviceSnapshot& d) {
    Value result=Value::object();for(unsigned i=0;i<FieldCount;++i)if(i<=Electrical&&d.has(static_cast<Field>(i)))result.fields[names[i]]=fieldValue(d,static_cast<Field>(i));return result;
}
Value snapshot(const Device& d) { return snapshot(*capture(d)); }
Catalog::Catalog():ordered(new SnapshotList),names(new NameIndex),groups(new SceneCatalog) {}
const DeviceSnapshot* Catalog::resolve(const std::string& target,std::string* error) const {
    if(target.size()==40&&target.compare(0,7,"matter:")==0){
        const DeviceSnapshot* primary=0;unsigned count=0;
        for(std::map<std::string,SnapshotPtr>::const_iterator i=devices.begin();i!=devices.end();++i)
            if(i->first.compare(0,target.size(),target)==0&&i->first.size()>target.size()&&(i->second->metadata->actor||i->second->metadata->light)){primary=i->second.get();++count;}
        if(count==1)return primary;
    }
    std::map<std::string,SnapshotPtr>::const_iterator direct=devices.find(target);
    if(direct!=devices.end())return direct->second.get();
    NameIndex::const_iterator name=names->find(folded(target));
    if(name==names->end()){*error="unknown device: "+target;return 0;}
    if(name->second.empty()){*error="ambiguous name; use stable IDs";return 0;}
    return resolve(name->second,error);
}
Value eventValue(const Event& e) {
    if(!e.device)return e.data;
    Value out=Value::object();out.fields["type"]=Value(e.type);out.fields["device"]=snapshot(*e.device);
    out.fields["device_id"]=Value(e.device->metadata->id);out.fields["source"]=Value(e.device->metadata->source);
    out.fields["at"]=Value(e.at);char chain[32];snprintf(chain,sizeof(chain),"%llu",static_cast<unsigned long long>(e.chain));
    out.fields["chain_id"]=Value(chain);out.fields["chain_depth"]=Value(static_cast<double>(e.depth));
    if(e.field!=FieldCount){out.fields["field"]=Value(fieldName(e.field));out.fields["value"]=fieldValue(*e.device,e.field);}
    if(e.hasOld)out.fields["old"]=e.previous?fieldValue(*e.previous,e.field):e.old;
    if(e.type=="device.removed")out.fields["reason"]=Value("removed");
    if(e.type=="remote.action"){
        out.fields["action"]=Value(e.action);out.fields["endpoint"]=Value(static_cast<double>(e.endpoint));
        if(e.position>=0)out.fields["position"]=Value(static_cast<double>(e.position));
        if(e.pressCount>=0)out.fields["press_count"]=Value(static_cast<double>(e.pressCount));
    }
    return out;
}

}
