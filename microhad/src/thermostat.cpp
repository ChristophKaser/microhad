#include "thermostat.h"
#include "gateway.h"
#include "json_util.h"
#include "bounded_json.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace {
/* SONOFF TRVZB mapping, independently encoded from the ZCL specification and
   zigbee-herdsman-converters/src/devices/sonoff.ts (TRVZB definition). The
   FC11 cluster uses ordinary ZCL frames: it does not require a manufacturer header. */
const ThermostatAttribute attributes[] = {
#define THERMOSTAT_ATTRIBUTE(name, cluster, attribute, type, scale, minimum, maximum, step, writable, version) \
    {name, cluster, attribute, type, scale, minimum, maximum, step, writable, version},
#include "../../config/thermostat-attributes.def"
#undef THERMOSTAT_ATTRIBUTE
};
unsigned int width(unsigned char type) {
    return type == 0x23 ? 4 : (type == 0x29 || type == 0x21 || type == 0x19 ? 2 : 1);
}
void put16(std::vector<unsigned char>* bytes, unsigned int value) {
    bytes->push_back(static_cast<unsigned char>(value)); bytes->push_back(static_cast<unsigned char>(value >> 8));
}
unsigned short get16(const std::vector<unsigned char>& bytes, unsigned long at) {
    return bytes[at] | (static_cast<unsigned short>(bytes[at + 1]) << 8);
}
bool versionAtLeast(std::string actual, const char* required) {
    if (!actual.empty() && (actual[0] == 'v' || actual[0] == 'V')) actual.erase(0, 1);
    int a=0,b=0,c=0,x=0,y=0,z=0;
    if (sscanf(actual.c_str(), "%d.%d.%d", &a,&b,&c) != 3 || sscanf(required, "%d.%d.%d", &x,&y,&z) != 3) return false;
    return a > x || (a == x && (b > y || (b == y && c >= z)));
}
}
const ThermostatAttribute* thermostatAttributes(unsigned long* count) {
    *count = sizeof(attributes)/sizeof(attributes[0]); return attributes;
}
const ThermostatAttribute* thermostatAttribute(const std::string& name) {
    unsigned long count; thermostatAttributes(&count);
    for (unsigned long i=0;i<count;++i) if (name == attributes[i].name) return &attributes[i];
    return 0;
}
bool isTrvzb(const Device& device) { return device.source == "zigbee" && device.modelId == "TRVZB"; }
bool isThermostat(const Device& device) {
    if (isTrvzb(device)) return true;
    if (device.source=="matter") return device.matterEndpoint>=0 &&
        std::find(device.matterServers.begin(),device.matterServers.end(),513U)!=device.matterServers.end();
    if (device.source!="zigbee") return false;
    for (unsigned long i=0;i<device.endpoints.size();++i)
        if (std::find(device.endpoints[i].inputClusters.begin(),device.endpoints[i].inputClusters.end(),0x0201)!=device.endpoints[i].inputClusters.end()) return true;
    return false;
}
bool thermostatSupported(const Device& device, const ThermostatAttribute& attribute) {
    if (!isThermostat(device) || device.thermostat.unsupported.count(attribute.name)) return false;
    if (attribute.cluster!=0x0201 && !isTrvzb(device)) return false;
    if (!isTrvzb(device)) {
        std::string name=attribute.name;
        bool heat=name.find("heating_setpoint")!=std::string::npos || name.find("heat_setpoint_limit")!=std::string::npos || name=="pi_heating_demand";
        bool cool=name.find("cooling_setpoint")!=std::string::npos || name.find("cool_setpoint_limit")!=std::string::npos || name=="pi_cooling_demand";
        std::map<std::string,double>::const_iterator sequence=device.thermostat.values.find("control_sequence_of_operation");
        std::map<std::string,double>::const_iterator features=device.thermostat.values.find("feature_map");
        if(device.source=="matter" && features!=device.thermostat.values.end()) {
            unsigned long mask=static_cast<unsigned long>(features->second);
            if((heat && !(mask&1)) || (cool && !(mask&2)))return false;
        } else if(sequence!=device.thermostat.values.end()) {
            int n=static_cast<int>(sequence->second);
            if((heat && n<2) || (cool && (n==2 || n==3)))return false;
        }
        return device.thermostat.attributesKnown ? device.thermostat.attributes.count(attribute.attribute)>0 :
            device.thermostat.values.count(attribute.name)>0;
    }
    if (attribute.cluster==0x0201 && std::string(attribute.name)!="local_temperature" &&
        std::string(attribute.name)!="occupied_heating_setpoint" && std::string(attribute.name)!="local_temperature_calibration" &&
        std::string(attribute.name)!="system_mode" && std::string(attribute.name)!="running_state") return device.thermostat.values.count(attribute.name)>0;
    return !attribute.minimumVersion[0] || device.thermostat.values.count(attribute.name) ||
        versionAtLeast(device.softwareBuildId, attribute.minimumVersion);
}
double thermostatValue(const Device& d, const std::string& name, double fallback) {
    std::map<std::string,double>::const_iterator it=d.thermostat.values.find(name);
    return it==d.thermostat.values.end() ? fallback : it->second;
}
ThermostatAttribute thermostatDefinition(const Device& d, const ThermostatAttribute& base) {
    ThermostatAttribute a=base; std::string name=a.name;
    if (name.find("setpoint")!=std::string::npos && name.find("limit")==std::string::npos && name!="min_setpoint_dead_band") {
        bool heat=name.find("heating")!=std::string::npos;
        std::string side=heat ? "heat" : "cool";
        a.minimum=thermostatValue(d,"min_"+side+"_setpoint_limit",thermostatValue(d,"abs_min_"+side+"_setpoint_limit",heat?7:16));
        a.maximum=thermostatValue(d,"max_"+side+"_setpoint_limit",thermostatValue(d,"abs_max_"+side+"_setpoint_limit",heat?30:32));
        if (isTrvzb(d)) { a.minimum=4; a.maximum=35; a.step=.5; }
    }
    return a;
}
bool thermostatModeSupported(const Device& d, int mode) {
    if (isTrvzb(d)) return mode==0 || mode==1 || mode==4;
    int sequence=static_cast<int>(thermostatValue(d,"control_sequence_of_operation",-1));
    bool heat=sequence>=0 ? sequence>=2 : thermostatSupported(d,*thermostatAttribute("occupied_heating_setpoint"));
    bool cool=sequence>=0 ? (sequence<2 || sequence>=4) : thermostatSupported(d,*thermostatAttribute("occupied_cooling_setpoint"));
    if (d.source=="matter") {
        unsigned long features=static_cast<unsigned long>(thermostatValue(d,"feature_map",0));
        heat=(features&1)!=0; cool=(features&2)!=0;
        if (mode==1) return heat && cool && (features&32)!=0;
    }
    return mode==0 || (mode==1 && heat && cool) || (mode==3 && cool) || (mode==4 && heat);
}
bool thermostatNumber(const std::string& text, double* value) {
    if (text.empty()) return false;
    char* end=0; *value=strtod(text.c_str(), &end);
    return end != text.c_str() && *end == 0 && *value == *value && *value >= -1e10 && *value <= 1e10;
}
bool thermostatEncode(const Device& device, const std::string& field, const std::string& text,
                      std::vector<unsigned char>* payload, std::string* error) {
    const ThermostatAttribute* base=thermostatAttribute(field);
    ThermostatAttribute definition; if (base) definition=thermostatDefinition(device,*base);
    const ThermostatAttribute* a=base ? &definition : 0; double number=0;
    if (!a || !a->writable || !thermostatSupported(device,*a)) { *error="unsupported thermostat setting"; return false; }
    if (!thermostatNumber(text,&number) || number<a->minimum || number>a->maximum) { *error="thermostat value outside supported range"; return false; }
    /* Calibration is signed tenths; a 0.2 degree increment is relative to zero. */
    double steps=number/a->step;
    if (std::fabs(steps-std::floor(steps+.5))>.00001 && field!="local_temperature_calibration") {
        *error="invalid thermostat increment"; return false;
    }
    if (field=="local_temperature_calibration" && std::fabs(number*10-std::floor(number*10+.5))>.00001) {
        *error="calibration requires whole tenths"; return false;
    }
    if ((field=="system_mode" && !thermostatModeSupported(device,static_cast<int>(number))) ||
        (field=="temperature_sensor_select" && number>1)) { *error="invalid thermostat mode"; return false; }
    std::map<std::string,double>::const_iterator smart=device.thermostat.values.find("smart_temperature_control");
    if (smart!=device.thermostat.values.end() && smart->second==2 &&
        (field=="valve_opening_degree" || field=="temperature_accuracy")) { *error="disable smart temperature control first"; return false; }
    std::map<std::string,double>::const_iterator mode=device.thermostat.values.find("temporary_mode_select");
    std::map<std::string,double>::const_iterator duration=device.thermostat.values.find("temporary_mode_duration");
    if ((field=="temporary_mode_duration" && number>180 && (mode==device.thermostat.values.end() || mode->second!=1)) ||
        (field=="temporary_mode_select" && number==0 && duration!=device.thermostat.values.end() && duration->second>180)) {
        *error="boost duration must not exceed 180 minutes"; return false;
    }
    if (field.find("setpoint")!=std::string::npos && field.find("limit")==std::string::npos && field!="min_setpoint_dead_band") {
        bool heat=field.find("heating")!=std::string::npos;
        std::string other=field.find("unoccupied")==0 ? "unoccupied_" : "occupied_";
        other+=heat ? "cooling_setpoint" : "heating_setpoint";
        std::map<std::string,double>::const_iterator peer=device.thermostat.values.find(other);
        double deadband=thermostatValue(device,"min_setpoint_dead_band",2.5);
        if (peer!=device.thermostat.values.end() && thermostatSupported(device,*thermostatAttribute(other)) && (heat ? number+deadband>peer->second : number-deadband<peer->second)) {
            *error="setpoint violates the heating/cooling deadband"; return false;
        }
    }
    if(field.find("setpoint_limit")!=std::string::npos) {
        bool heat=field.find("heat")!=std::string::npos, minimum=field.find("min_")==0;
        std::string side=heat?"heat":"cool";
        double low=thermostatValue(device,"abs_min_"+side+"_setpoint_limit",heat?7:16);
        double high=thermostatValue(device,"abs_max_"+side+"_setpoint_limit",heat?30:32);
        if(number<low || number>high || (minimum ? number>thermostatValue(device,"max_"+side+"_setpoint_limit",high) : number<thermostatValue(device,"min_"+side+"_setpoint_limit",low))) {
            *error="invalid thermostat setpoint limits"; return false;
        }
    }
    long raw=static_cast<long>(std::floor(number*a->scale+.5));
    payload->clear(); put16(payload,a->attribute); payload->push_back(a->type);
    for (unsigned int i=0;i<width(a->type);++i) payload->push_back(static_cast<unsigned char>(static_cast<unsigned long>(raw)>>(8*i)));
    return true;
}
bool applyThermostatAttribute(Device* device, unsigned short cluster, unsigned short attribute,
                             unsigned char type, const std::vector<unsigned char>& value, long now) {
    if (!isThermostat(*device) || (cluster!=0x0201 && !isTrvzb(*device))) return false;
    unsigned long count; thermostatAttributes(&count);
    for (unsigned long i=0;i<count;++i) {
        const ThermostatAttribute& a=attributes[i];
        if (a.cluster!=cluster || a.attribute!=attribute || a.type!=type || value.size()!=width(type)) continue;
        unsigned long raw=0;
        for (unsigned long b=0;b<value.size();++b) raw |= static_cast<unsigned long>(value[b])<<(8*b);
        double number=type==0x29 ? static_cast<double>(static_cast<short>(raw))/a.scale :
            type==0x28 ? static_cast<double>(static_cast<signed char>(raw))/a.scale : static_cast<double>(raw)/a.scale;
        if (cluster==0xfc11 && attribute==0x600e && raw==3) number=1; // remote source reconnected
        bool inactive=cluster==0xfc11 && attribute==0x6014 && raw==255;
        if ((!inactive && (number<a.minimum || number>a.maximum)) ||
            (std::string(a.name)=="system_mode" && number!=0 && number!=1 && number!=3 && number!=4 && number!=5 && number!=6 && number!=7 && number!=8 && number!=9) ||
            (std::string(a.name)=="smart_temperature_control" && number!=0 && number!=2)) return false;
        device->thermostat.unsupported.erase(a.name);
        device->thermostat.values[a.name]=number;
        device->thermostat.pending.erase(a.name);
        device->kind="thermostat"; device->hasLight=false; device->hasBrightness=false; device->hasActor=false;
        if (std::string(a.name)=="local_temperature") {
            device->hasTemperature=true; device->temperatureKnown=true;
            device->temperatureCelsius=number; device->temperatureObservation.at=now;
        }
        device->lastSeen=now; return true;
    }
    return false;
}
bool thermostatSchedule(const std::string& text, int day, std::vector<unsigned char>* payload) {
    if (day<0 || day>6) return false;
    std::istringstream input(text); std::string token;
    std::vector<unsigned char> transitions; int previous=-1;
    while (input>>token) {
        if (token.size()<7 || token[2]!=':' || token[5]!='/' || token[0]<'0' || token[0]>'2' ||
            token[1]<'0' || token[1]>'9' || token[3]<'0' || token[3]>'5' || token[4]<'0' || token[4]>'9') return false;
        int hour=(token[0]-'0')*10+token[1]-'0', minute=(token[3]-'0')*10+token[4]-'0';
        int at=hour*60+minute; double temp;
        if (hour>23 || at<=previous || (previous<0 && at!=0) || !thermostatNumber(token.substr(6),&temp) ||
            temp<4 || temp>35 || std::fabs(temp*2-std::floor(temp*2+.5))>.00001 || transitions.size()>=24) return false;
        put16(&transitions,at); put16(&transitions,static_cast<unsigned int>(temp*100)); previous=at;
    }
    if (transitions.empty()) return false;
    payload->clear(); payload->push_back(static_cast<unsigned char>(transitions.size()/4));
    payload->push_back(static_cast<unsigned char>(1<<day)); payload->push_back(1);
    payload->insert(payload->end(),transitions.begin(),transitions.end()); return true;
}
bool thermostatScheduleForDevice(const Device& device, const std::string& text, int day, std::vector<unsigned char>* payload) {
    if (day<0 || day>6 || !isThermostat(device)) return false;
    if (isTrvzb(device) && (text.empty() || text[0]!='{')) return thermostatSchedule(text,day,payload);
    unsigned int mode=1; std::vector<std::string> entries;
    if (!text.empty() && text[0]=='{') {
        JsonFields root;
        if (!BoundedJson(text).parse(&root) || root.size()!=2 || !root.count("mode") || !root.count("transitions")) return false;
        double m; if (!thermostatNumber(root["mode"],&m) || m<1 || m>3 || m!=std::floor(m)) return false;
        mode=static_cast<unsigned int>(m);
        if (!BoundedJson(root["transitions"]).parseArray(&entries)) return false;
    } else {
        std::istringstream input(text); std::string token;
        while (input>>token) {
            if (token.size()<7 || token[2]!=':' || token[5]!='/') return false;
            int hh=0, mm=0; if (sscanf(token.substr(0,5).c_str(),"%d:%d",&hh,&mm)!=2 || hh<0 || hh>23 || mm<0 || mm>59) return false;
            std::ostringstream entry; entry<<"{\"time\":"<<hh*60+mm<<",\"heat\":"<<token.substr(6)<<'}'; entries.push_back(entry.str());
        }
    }
    unsigned int maximum=isTrvzb(device) ? 6 : static_cast<unsigned int>(thermostatValue(device,"number_of_daily_transitions",thermostatValue(device,"number_of_weekly_transitions",10)));
    if (entries.empty() || entries.size()>maximum || entries.size()>32 || (isTrvzb(device) && mode!=1)) return false;
    if (!isTrvzb(device) && !device.thermostat.scheduleSupported) return false;
    payload->clear(); payload->push_back(static_cast<unsigned char>(entries.size())); payload->push_back(static_cast<unsigned char>(1<<day)); payload->push_back(mode);
    int previous=-1;
    for (unsigned long i=0;i<entries.size();++i) {
        JsonFields entry; double at;
        if (!BoundedJson(entries[i]).parse(&entry) || entry.size()!=1+((mode&1)?1U:0U)+((mode&2)?1U:0U) ||
            !thermostatNumber(entry["time"],&at) || at<0 || at>=1440 || at!=std::floor(at) || at<=previous ||
            (isTrvzb(device) && i==0 && at!=0)) return false;
        put16(payload,static_cast<unsigned int>(at)); previous=static_cast<int>(at);
        double heat=0,cool=0;
        for (unsigned int bit=1;bit<=2;bit<<=1) if (mode&bit) {
            std::string name=bit==1 ? "occupied_heating_setpoint" : "occupied_cooling_setpoint";
            ThermostatAttribute a=thermostatDefinition(device,*thermostatAttribute(name)); double value;
            if (!thermostatSupported(device,*thermostatAttribute(name)) || !thermostatNumber(entry[bit==1?"heat":"cool"],&value) || value<a.minimum || value>a.maximum ||
                std::fabs(value/a.step-std::floor(value/a.step+.5))>.00001) return false;
            put16(payload,static_cast<unsigned short>(static_cast<short>(std::floor(value*100+.5))));
            if(bit==1)heat=value;else cool=value;
        }
        if(mode==3 && heat+thermostatValue(device,"min_setpoint_dead_band",2.5)>cool) return false;
    }
    return true;
}
bool applyThermostatSchedule(Device* device, const std::vector<unsigned char>& payload) {
    if (!isThermostat(*device) || payload.size()<3 || payload[0]>32 || !payload[1] || (payload[1]&128) || !payload[2] || payload[2]>3) return false;
    unsigned int width=2+((payload[2]&1)?2:0)+((payload[2]&2)?2:0);
    if (payload.size()!=3UL+width*payload[0]) return false;
    std::ostringstream normalized,legacy; normalized<<"{\"mode\":"<<static_cast<unsigned int>(payload[2])<<",\"transitions\":[";
    int previous=-1;
    for (unsigned int i=0;i<payload[0];++i) {
        unsigned int at=get16(payload,3+i*width), offset=3+i*width+2;
        if (at>=1440 || static_cast<int>(at)<=previous) return false;
        previous=at; if(i) { normalized<<','; legacy<<' '; }
        normalized<<"{\"time\":"<<at;
        legacy<<std::setfill('0')<<std::setw(2)<<at/60<<':'<<std::setw(2)<<at%60<<'/';
        if(payload[2]&1) { double value=static_cast<short>(get16(payload,offset))/100.0; offset+=2; normalized<<",\"heat\":"<<value; legacy<<value; }
        if(payload[2]&2) { double value=static_cast<short>(get16(payload,offset))/100.0; normalized<<",\"cool\":"<<value; }
        normalized<<'}';
    }
    normalized<<"]}";
    if(isTrvzb(*device) && payload[0]) { std::vector<unsigned char> checked; if(!thermostatScheduleForDevice(*device,normalized.str(),0,&checked))return false; }
    device->thermostat.scheduleSupported=true;
    for (int day=0;day<7;++day) if (payload[1]&(1<<day)) {
        device->thermostat.schedules[day]=payload[2]==1 ? legacy.str() : normalized.str();
        device->thermostat.scheduleDetails[day]=normalized.str();
        std::ostringstream field; field<<"schedule_"<<day; device->thermostat.pending.erase(field.str());
    }
    return true;
}
bool thermostatWireValue(unsigned int attribute, const std::string& kind, const std::string& value) {
    if (kind=="thermostat_list" && (attribute==65531 || attribute==65529)) {
        std::vector<std::string> values; if (!BoundedJson(value).parseArray(&values))return false;
        std::set<unsigned int> unique;
        for(unsigned long i=0;i<values.size();++i) { double number; if(!thermostatNumber(values[i],&number) || number<0 || number>0xffffffffUL || number!=std::floor(number) || !unique.insert(static_cast<unsigned int>(number)).second)return false; }
        return true;
    }
    if (kind=="thermostat_schedule" && attribute>=65536 && attribute<65543) {
        JsonFields fields; std::vector<std::string> transitions;
        if (!BoundedJson(value).parse(&fields) || fields.size()!=2 || !fields.count("mode") || !fields.count("transitions") ||
            (fields["mode"]!="1" && fields["mode"]!="2" && fields["mode"]!="3") || !BoundedJson(fields["transitions"]).parseArray(&transitions) || transitions.size()>32) return false;
        const unsigned int mode=static_cast<unsigned int>(fields["mode"][0]-'0');
        double previous=-1;
        for (unsigned long i=0;i<transitions.size();++i) {
            JsonFields row; double at;
            if (!BoundedJson(transitions[i]).parse(&row) || row.size()!=1+((mode&1)?1U:0U)+((mode&2)?1U:0U) ||
                !thermostatNumber(row["time"],&at) || at<0 || at>=1440 || at!=std::floor(at) || at<=previous) return false;
            previous=at;
            for (unsigned int bit=1;bit<=2;bit<<=1) if (mode&bit) {
                double temperature;
                if (!thermostatNumber(row[bit==1?"heat":"cool"],&temperature) || temperature<-273.15 || temperature>327.67 ||
                    std::fabs(temperature*100-std::floor(temperature*100+.5))>.00001) return false;
            }
        }
        return true;
    }
    if (kind!="thermostat")return false;
    unsigned long count; const ThermostatAttribute* list=thermostatAttributes(&count);
    for(unsigned long i=0;i<count;++i) if(list[i].cluster==513 && list[i].attribute==attribute) {
        double number; if(value=="null")return true;
        return thermostatNumber(value,&number) && number==std::floor(number) && number/list[i].scale>=list[i].minimum && number/list[i].scale<=list[i].maximum;
    }
    return false;
}
bool applyMatterThermostat(Device* device, unsigned int attribute, const std::string& kind, const std::string& value, long now) {
    if(!isThermostat(*device) || !thermostatWireValue(attribute,kind,value))return false;
    if(kind=="thermostat_list") {
        std::vector<std::string> values; BoundedJson(value).parseArray(&values);
        std::set<unsigned int>& target=attribute==65531 ? device->thermostat.attributes : device->thermostat.commands; target.clear();
        for(unsigned long i=0;i<values.size();++i) target.insert(static_cast<unsigned int>(strtoul(values[i].c_str(),0,10)));
        if(attribute==65531) device->thermostat.attributesKnown=true;
        else { device->thermostat.commandsKnown=true; device->thermostat.scheduleSupported=target.count(1) && target.count(2); }
        return true;
    }
    if(kind=="thermostat_schedule") {
        std::vector<unsigned char> bytes;
        JsonFields fields; std::vector<std::string> entries;
        if(!BoundedJson(value).parse(&fields) || !BoundedJson(fields["transitions"]).parseArray(&entries))return false;
        bool supported=device->thermostat.scheduleSupported; device->thermostat.scheduleSupported=true;
        if(!entries.empty() && !thermostatScheduleForDevice(*device,value,attribute-65536,&bytes)) { device->thermostat.scheduleSupported=supported;return false; }
        if(entries.empty()) {bytes.push_back(0);bytes.push_back(static_cast<unsigned char>(1<<(attribute-65536)));bytes.push_back(static_cast<unsigned char>(atoi(fields["mode"].c_str())));}
        return applyThermostatSchedule(device,bytes);
    }
    unsigned long count; const ThermostatAttribute* list=thermostatAttributes(&count);
    for(unsigned long i=0;i<count;++i) if(list[i].cluster==513 && list[i].attribute==attribute) {
        if(value=="null") { device->thermostat.values.erase(list[i].name); if(attribute==0){device->temperatureKnown=false;device->temperatureObservation.at=0;}return true; }
        unsigned long raw=(list[i].type==0x28 || list[i].type==0x29) ? static_cast<unsigned long>(strtol(value.c_str(),0,10)) : strtoul(value.c_str(),0,10); std::vector<unsigned char> bytes;
        for(unsigned int b=0;b<width(list[i].type);++b) bytes.push_back(static_cast<unsigned char>(static_cast<unsigned long>(raw)>>(8*b)));
        return applyThermostatAttribute(device,513,attribute,list[i].type,bytes,now);
    }
    return false;
}
bool thermostatValidateSetting(const Device& device, const std::string& field,
                               const std::string& value, std::string* error) {
    if(!isThermostat(device)) { *error="device is not a thermostat"; return false; }
    if(field=="stale_minutes" && device.source!="matter") {
        double minutes=0;
        if(!thermostatNumber(value,&minutes)||minutes<5||minutes>1440||minutes!=std::floor(minutes)) {
            *error="stale timeout must be 5–1440 whole minutes"; return false;
        }
        return true;
    }
    if(field=="temperature_source" && device.source!="matter") {
        if(!thermostatSupported(device,*thermostatAttribute("temperature_sensor_select")) ||
           (value!="builtin"&&!thermostatSupported(device,*thermostatAttribute("external_temperature_input"))) || value.empty()) {
            *error="external temperature is unavailable"; return false;
        }
        return true;
    }
    std::vector<unsigned char> payload;
    if(field.size()==10&&field.compare(0,9,"schedule_")==0&&field[9]>='0'&&field[9]<='6') {
        if(value=="__read__" ? (isTrvzb(device)||device.thermostat.scheduleSupported) :
           thermostatScheduleForDevice(device,value,field[9]-'0',&payload))return true;
        *error="invalid or unsupported thermostat schedule";return false;
    }
    if(!device.thermostat.sourceId.empty()&&(field=="external_temperature_input"||field=="temperature_sensor_select")) {
        *error="switch to manual or built-in temperature source first";return false;
    }
    return thermostatEncode(device,field,value,&payload,error);
}

std::string thermostatJson(const Device& device) {
    std::ostringstream out; const ThermostatState& t=device.thermostat;
    out<<"{\"values\":{";
    for (std::map<std::string,double>::const_iterator it=t.values.begin();it!=t.values.end();++it) {
        if (it!=t.values.begin()) out<<',';
        out<<'"'<<it->first<<"\":"<<it->second;
    }
    out<<"},\"supported\":{"; unsigned long count; thermostatAttributes(&count);
    for (unsigned long i=0;i<count;++i) { if(i)out<<','; out<<'"'<<attributes[i].name<<"\":"<<(thermostatSupported(device,attributes[i])?"true":"false"); }
    out<<"},\"fields\":{";
    for(unsigned long i=0;i<count;++i) { if(i)out<<','; ThermostatAttribute a=thermostatDefinition(device,attributes[i]);
        out<<'"'<<a.name<<"\":{\"min\":"<<a.minimum<<",\"max\":"<<a.maximum<<",\"step\":"<<a.step<<",\"writable\":"<<(a.writable?"true":"false")<<'}'; }
    out<<"},\"modes\":["; bool firstMode=true;
    for(int mode=0;mode<=9;++mode) if(thermostatModeSupported(device,mode)) { if(!firstMode)out<<','; firstMode=false;out<<mode; }
    out<<"],\"schedule_supported\":"<<((isTrvzb(device)||t.scheduleSupported)?"true":"false")<<",\"schedule_max_transitions\":"<<(isTrvzb(device)?6:static_cast<int>(thermostatValue(device,"number_of_daily_transitions",thermostatValue(device,"number_of_weekly_transitions",10))))
       <<",\"schedule_midnight_start\":"<<(isTrvzb(device)?"true":"false")<<",\"external_input_supported\":"<<(isTrvzb(device)?"true":"false")
       <<",\"schedule_details\":[";
    for(int day=0;day<7;++day) {if(day)out<<',';std::map<int,std::string>::const_iterator it=t.scheduleDetails.find(day);out<<(it==t.scheduleDetails.end()?"null":it->second);}
    out<<"],\"pending\":[";
    for (std::set<std::string>::const_iterator it=t.pending.begin();it!=t.pending.end();++it) {
        if (it!=t.pending.begin()) out<<',';
        out<<'"'<<jsonEscape(*it)<<'"';
    }
    out<<"],\"schedules\":[";
    for (int day=0;day<7;++day) {
        if(day)out<<',';
        std::map<int,std::string>::const_iterator it=t.schedules.find(day);
        if (it==t.schedules.end()) out<<"null"; else out<<'"'<<jsonEscape(it->second)<<'"';
    }
    out<<"],\"source_id\":\""<<jsonEscape(t.sourceId)<<"\",\"stale_minutes\":"<<t.staleMinutes
       <<",\"source_observed_at\":"<<t.sourceObservedAt<<",\"status\":\""<<jsonEscape(t.status)
       <<"\",\"error\":\""<<jsonEscape(t.error)<<"\"}";
    return out.str();
}

// Matter IPC delivers typed attribute values; the JSON helper above is only
// for public/configuration inputs. Neither scalar nor schedule reports reparse JSON.
bool thermostatWireValue(unsigned int attribute,const MatterProto::AttributeValue& a) {
    if(!a.type.present||!a.value.present){ return false; }
    const MatterProto::ReadingValue& v=a.value.value;
    if(a.type.value=="thermostat_list"&&(attribute==65531||attribute==65529)){
        if(!v.list.present||v.list.value.items.size()>512){ return false; }
        std::set<uint32_t> unique(v.list.value.items.begin(),v.list.value.items.end());
        return unique.size()==v.list.value.items.size();
    }
    if(a.type.value=="thermostat_schedule"&&attribute>=65536&&attribute<65543){
        if(!v.measurement.present){ return false; }const MatterProto::Measurement& m=v.measurement.value;
        if(!m.mode.present||m.mode.value<1||m.mode.value>3||!m.transitions.present||m.transitions.value.items.size()>32||m.value.present||m.unit.present||m.quality.present){ return false; }
        int previous=-1;
        for(size_t i=0;i<m.transitions.value.items.size();++i){const MatterProto::ThermostatTransition& t=m.transitions.value.items[i];
            if(!t.time.present||t.time.value>=1440||static_cast<int>(t.time.value)<=previous||t.heat.present!=bool(m.mode.value&1)||t.cool.present!=bool(m.mode.value&2)){ return false; }
            previous=t.time.value;const double values[]={t.heat.value,t.cool.value};
            for(unsigned n=0;n<2;++n)if(m.mode.value&(1U<<n)){const double x=values[n];if(!std::isfinite(x)||x< -273.15||x>327.67||std::fabs(x*100-std::floor(x*100+.5))>.00001){ return false; }}
        }return true;
    }
    if(a.type.value!="thermostat"){ return false; }
    unsigned long count;const ThermostatAttribute* list=thermostatAttributes(&count);
    for(unsigned long i=0;i<count;++i)if(list[i].cluster==513&&list[i].attribute==attribute){
        if(v.null_value.present)return v.null_value.value;
        if(!v.integer.present&&!v.unsigned_value.present){ return false; }
        const double raw=v.integer.present?static_cast<double>(v.integer.value):static_cast<double>(v.unsigned_value.value);
        return raw/list[i].scale>=list[i].minimum&&raw/list[i].scale<=list[i].maximum;
    }{ return false; }
}
bool applyMatterThermostat(Device* d,unsigned int attribute,const MatterProto::AttributeValue& a,long now){
    if(!isThermostat(*d)||!thermostatWireValue(attribute,a)){ return false; }
    const MatterProto::ReadingValue& v=a.value.value;
    if(a.type.value=="thermostat_list"){
        std::set<unsigned int>& target=attribute==65531?d->thermostat.attributes:d->thermostat.commands;
        target.clear();target.insert(v.list.value.items.begin(),v.list.value.items.end());
        if(attribute==65531)d->thermostat.attributesKnown=true;else{d->thermostat.commandsKnown=true;d->thermostat.scheduleSupported=target.count(1)&&target.count(2);}return true;
    }
    if(a.type.value=="thermostat_schedule"){
        const MatterProto::Measurement& m=v.measurement.value;const std::vector<MatterProto::ThermostatTransition>& entries=m.transitions.value.items;
        const unsigned maximum=isTrvzb(*d)?6:static_cast<unsigned>(thermostatValue(*d,"number_of_daily_transitions",thermostatValue(*d,"number_of_weekly_transitions",10)));
        if(entries.size()>maximum||(isTrvzb(*d)&&(m.mode.value!=1||(!entries.empty()&&entries[0].time.value!=0)))){ return false; }
        std::vector<unsigned char> bytes;bytes.push_back(entries.size());bytes.push_back(1U<<(attribute-65536));bytes.push_back(m.mode.value);
        for(size_t i=0;i<entries.size();++i){const MatterProto::ThermostatTransition& t=entries[i];put16(&bytes,t.time.value);
            for(unsigned bit=1;bit<=2;bit<<=1)if(m.mode.value&bit){const ThermostatAttribute* base=thermostatAttribute(bit==1?"occupied_heating_setpoint":"occupied_cooling_setpoint");const ThermostatAttribute def=thermostatDefinition(*d,*base);const double value=bit==1?t.heat.value:t.cool.value;
                if(!thermostatSupported(*d,*base)||value<def.minimum||value>def.maximum||std::fabs(value/def.step-std::floor(value/def.step+.5))>.00001){ return false; }
                put16(&bytes,static_cast<unsigned>(static_cast<long>(std::floor(value*100+.5))));
            }
        }d->thermostat.scheduleSupported=true;return applyThermostatSchedule(d,bytes);
    }
    unsigned long count;const ThermostatAttribute* list=thermostatAttributes(&count);
    for(unsigned long i=0;i<count;++i)if(list[i].cluster==513&&list[i].attribute==attribute){
        if(v.null_value.present){d->thermostat.values.erase(list[i].name);if(attribute==0){d->temperatureKnown=false;d->temperatureObservation.at=0;}return true;}
        const uint64_t raw=v.integer.present?static_cast<uint64_t>(v.integer.value):v.unsigned_value.value;std::vector<unsigned char> bytes;
        for(unsigned b=0;b<width(list[i].type);++b)bytes.push_back(static_cast<unsigned char>(raw>>(8*b)));
        return applyThermostatAttribute(d,513,attribute,list[i].type,bytes,now);
    }{ return false; }
}
