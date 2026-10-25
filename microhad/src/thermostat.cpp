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
    {#name, ThermostatField::name, cluster, attribute, type, scale, minimum, maximum, step, writable, version},
#include "../../config/thermostat-attributes.def"
#undef THERMOSTAT_ATTRIBUTE
};
bool heatingField(ThermostatField::Id id) {
    using namespace ThermostatField;
    return id==occupied_heating_setpoint || id==unoccupied_heating_setpoint ||
        id==abs_min_heat_setpoint_limit || id==abs_max_heat_setpoint_limit ||
        id==min_heat_setpoint_limit || id==max_heat_setpoint_limit || id==pi_heating_demand;
}
bool coolingField(ThermostatField::Id id) {
    using namespace ThermostatField;
    return id==occupied_cooling_setpoint || id==unoccupied_cooling_setpoint ||
        id==abs_min_cool_setpoint_limit || id==abs_max_cool_setpoint_limit ||
        id==min_cool_setpoint_limit || id==max_cool_setpoint_limit || id==pi_cooling_demand;
}
bool setpointField(ThermostatField::Id id) {
    using namespace ThermostatField;
    return id==occupied_heating_setpoint || id==unoccupied_heating_setpoint ||
        id==occupied_cooling_setpoint || id==unoccupied_cooling_setpoint;
}
bool limitField(ThermostatField::Id id) {
    return (heatingField(id)||coolingField(id)) && !setpointField(id) &&
        id!=ThermostatField::pi_heating_demand && id!=ThermostatField::pi_cooling_demand;
}
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
const ThermostatAttribute* thermostatAttribute(ThermostatField::Id id) {
    return id<ThermostatField::Count ? &attributes[id] : 0;
}
unsigned thermostatPendingId(const std::string& name) {
    if(name.size()==10 && name.compare(0,9,"schedule_")==0 && name[9]>='0' && name[9]<='6')
        return 2*ThermostatField::Count+name[9]-'0';
    const bool report=name.compare(0,7,"report:")==0;
    const ThermostatAttribute* a=thermostatAttribute(report?name.substr(7):name);
    return a ? static_cast<unsigned>(a->id)+(report?ThermostatField::Count:0) : static_cast<unsigned>(ThermostatPendingCount);
}
std::string thermostatPendingName(unsigned id) {
    if(id>=ThermostatPendingCount)return "";
    if(id>=2*ThermostatField::Count)return std::string("schedule_")+static_cast<char>('0'+id-2*ThermostatField::Count);
    return (id>=ThermostatField::Count?std::string("report:"):std::string())+attributes[id%ThermostatField::Count].name;
}
bool isTrvzb(const Device& device) { return device.source == "zigbee" && device.modelId == "TRVZB"; }
bool isThermostat(const Device& device) { return device.thermostat.present(); }
bool thermostatProfile(const Device& device) {
    if (isTrvzb(device)) return true;
    if (device.source=="matter") return device.matter.read().endpoint>=0 &&
        std::find(device.matter.read().servers.begin(),device.matter.read().servers.end(),513U)!=device.matter.read().servers.end();
    if (device.source!="zigbee") return false;
    for (unsigned long i=0;i<device.zigbee.read().endpoints.size();++i)
        if (std::find(device.zigbee.read().endpoints[i].inputClusters.begin(),device.zigbee.read().endpoints[i].inputClusters.end(),0x0201)!=device.zigbee.read().endpoints[i].inputClusters.end()) return true;
    return false;
}
bool thermostatSupported(const Device& device, const ThermostatAttribute& attribute) {
    if (!isThermostat(device) || device.thermostat.read().unsupported.count(attribute.id)) return false;
    if (attribute.cluster!=0x0201 && !isTrvzb(device)) return false;
    if (!isTrvzb(device)) {
        const bool heat=heatingField(attribute.id), cool=coolingField(attribute.id);
        const double sequence=device.thermostat.read().values.get(ThermostatField::control_sequence_of_operation,0);
        const bool sequenceKnown=device.thermostat.read().values.count(ThermostatField::control_sequence_of_operation);
        const double features=device.thermostat.read().values.get(ThermostatField::feature_map,0);
        const bool featuresKnown=device.thermostat.read().values.count(ThermostatField::feature_map);
        if(device.source=="matter" && featuresKnown) {
            unsigned long mask=static_cast<unsigned long>(features);
            if((heat && !(mask&1)) || (cool && !(mask&2)))return false;
        } else if(sequenceKnown) {
            int n=static_cast<int>(sequence);
            if((heat && n<2) || (cool && (n==2 || n==3)))return false;
        }
        return device.thermostat.read().attributesKnown ? device.thermostat.read().attributes.count(attribute.attribute)>0 :
            device.thermostat.read().values.count(attribute.id)>0;
    }
    if (attribute.cluster==0x0201 && attribute.id!=ThermostatField::local_temperature &&
        attribute.id!=ThermostatField::occupied_heating_setpoint && attribute.id!=ThermostatField::local_temperature_calibration &&
        attribute.id!=ThermostatField::system_mode && attribute.id!=ThermostatField::running_state) return device.thermostat.read().values.count(attribute.id)>0;
    return !attribute.minimumVersion[0] || device.thermostat.read().values.count(attribute.id) ||
        versionAtLeast(device.softwareBuildId, attribute.minimumVersion);
}
double thermostatValue(const Device& d, ThermostatField::Id id, double fallback) {
    return d.thermostat.read().values.get(id,fallback);
}
ThermostatAttribute thermostatDefinition(const Device& d, const ThermostatAttribute& base) {
    ThermostatAttribute a=base;
    if (setpointField(a.id)) {
        bool heat=heatingField(a.id);

        a.minimum=thermostatValue(d,(heat?ThermostatField::min_heat_setpoint_limit:ThermostatField::min_cool_setpoint_limit),thermostatValue(d,(heat?ThermostatField::abs_min_heat_setpoint_limit:ThermostatField::abs_min_cool_setpoint_limit),heat?7:16));
        a.maximum=thermostatValue(d,(heat?ThermostatField::max_heat_setpoint_limit:ThermostatField::max_cool_setpoint_limit),thermostatValue(d,(heat?ThermostatField::abs_max_heat_setpoint_limit:ThermostatField::abs_max_cool_setpoint_limit),heat?30:32));
        if (isTrvzb(d)) { a.minimum=4; a.maximum=35; a.step=.5; }
    }
    return a;
}
bool thermostatModeSupported(const Device& d, int mode) {
    if (isTrvzb(d)) return mode==0 || mode==1 || mode==4;
    int sequence=static_cast<int>(thermostatValue(d,ThermostatField::control_sequence_of_operation,-1));
    bool heat=sequence>=0 ? sequence>=2 : thermostatSupported(d,*thermostatAttribute(ThermostatField::occupied_heating_setpoint));
    bool cool=sequence>=0 ? (sequence<2 || sequence>=4) : thermostatSupported(d,*thermostatAttribute(ThermostatField::occupied_cooling_setpoint));
    if (d.source=="matter") {
        unsigned long features=static_cast<unsigned long>(thermostatValue(d,ThermostatField::feature_map,0));
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
    const ThermostatField::Id id=a->id;
    /* Calibration is signed tenths; a 0.2 degree increment is relative to zero. */
    double steps=number/a->step;
    if (std::fabs(steps-std::floor(steps+.5))>.00001 && id!=ThermostatField::local_temperature_calibration) {
        *error="invalid thermostat increment"; return false;
    }
    if (id==ThermostatField::local_temperature_calibration && std::fabs(number*10-std::floor(number*10+.5))>.00001) {
        *error="calibration requires whole tenths"; return false;
    }
    if ((id==ThermostatField::system_mode && !thermostatModeSupported(device,static_cast<int>(number))) ||
        (id==ThermostatField::temperature_sensor_select && number>1)) { *error="invalid thermostat mode"; return false; }
    const double smart=device.thermostat.read().values.get(ThermostatField::smart_temperature_control,0);
    const bool smartKnown=device.thermostat.read().values.count(ThermostatField::smart_temperature_control);
    if (smartKnown && smart==2 &&
        (id==ThermostatField::valve_opening_degree || id==ThermostatField::temperature_accuracy)) { *error="disable smart temperature control first"; return false; }
    const double mode=device.thermostat.read().values.get(ThermostatField::temporary_mode_select,0);
    const bool modeKnown=device.thermostat.read().values.count(ThermostatField::temporary_mode_select);
    const double duration=device.thermostat.read().values.get(ThermostatField::temporary_mode_duration,0);
    const bool durationKnown=device.thermostat.read().values.count(ThermostatField::temporary_mode_duration);
    if ((id==ThermostatField::temporary_mode_duration && number>180 && (!modeKnown || mode!=1)) ||
        (id==ThermostatField::temporary_mode_select && number==0 && durationKnown && duration>180)) {
        *error="boost duration must not exceed 180 minutes"; return false;
    }
    if (setpointField(id)) {
        bool heat=heatingField(id);
        const ThermostatField::Id other=(id==ThermostatField::unoccupied_heating_setpoint || id==ThermostatField::unoccupied_cooling_setpoint) ?
            (heat?ThermostatField::unoccupied_cooling_setpoint:ThermostatField::unoccupied_heating_setpoint):
            (heat?ThermostatField::occupied_cooling_setpoint:ThermostatField::occupied_heating_setpoint);
        const double peer=device.thermostat.read().values.get(other,0);
        double deadband=thermostatValue(device,ThermostatField::min_setpoint_dead_band,2.5);
        if (device.thermostat.read().values.count(other) && thermostatSupported(device,*thermostatAttribute(other)) && (heat ? number+deadband>peer : number-deadband<peer)) {
            *error="setpoint violates the heating/cooling deadband"; return false;
        }
    }
    if(limitField(id)) {
        bool heat=heatingField(id), minimum=id==ThermostatField::min_heat_setpoint_limit || id==ThermostatField::min_cool_setpoint_limit;

        double low=thermostatValue(device,(heat?ThermostatField::abs_min_heat_setpoint_limit:ThermostatField::abs_min_cool_setpoint_limit),heat?7:16);
        double high=thermostatValue(device,(heat?ThermostatField::abs_max_heat_setpoint_limit:ThermostatField::abs_max_cool_setpoint_limit),heat?30:32);
        if(number<low || number>high || (minimum ? number>thermostatValue(device,(heat?ThermostatField::max_heat_setpoint_limit:ThermostatField::max_cool_setpoint_limit),high) : number<thermostatValue(device,(heat?ThermostatField::min_heat_setpoint_limit:ThermostatField::min_cool_setpoint_limit),low))) {
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
    if(thermostatProfile(*device))device->thermostat.ensure();
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
            (a.id==ThermostatField::system_mode && number!=0 && number!=1 && number!=3 && number!=4 && number!=5 && number!=6 && number!=7 && number!=8 && number!=9) ||
            (a.id==ThermostatField::smart_temperature_control && number!=0 && number!=2)) return false;
        device->thermostat.ensure().unsupported.erase(a.id);
        device->thermostat.ensure().values[a.id]=number;
        device->thermostat.ensure().pending.erase(a.id);
        device->kind="thermostat"; device->light.setPresent(false); device->brightness.setPresent(false); device->actor.setPresent(false);
        if (a.id==ThermostatField::local_temperature) {
            device->temperature.setPresent(true); device->temperature.ensure().known=true;
            device->temperature.ensure().celsius=number; device->temperature.ensure().observation.at=now;
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
    unsigned int maximum=isTrvzb(device) ? 6 : static_cast<unsigned int>(thermostatValue(device,ThermostatField::number_of_daily_transitions,thermostatValue(device,ThermostatField::number_of_weekly_transitions,10)));
    if (entries.empty() || entries.size()>maximum || entries.size()>32 || (isTrvzb(device) && mode!=1)) return false;
    if (!isTrvzb(device) && !device.thermostat.read().scheduleSupported) return false;
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
            const ThermostatField::Id name=bit==1 ? ThermostatField::occupied_heating_setpoint : ThermostatField::occupied_cooling_setpoint;
            ThermostatAttribute a=thermostatDefinition(device,*thermostatAttribute(name)); double value;
            if (!thermostatSupported(device,*thermostatAttribute(name)) || !thermostatNumber(entry[bit==1?"heat":"cool"],&value) || value<a.minimum || value>a.maximum ||
                std::fabs(value/a.step-std::floor(value/a.step+.5))>.00001) return false;
            put16(payload,static_cast<unsigned short>(static_cast<short>(std::floor(value*100+.5))));
            if(bit==1)heat=value;else cool=value;
        }
        if(mode==3 && heat+thermostatValue(device,ThermostatField::min_setpoint_dead_band,2.5)>cool) return false;
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
    device->thermostat.ensure().scheduleSupported=true;
    for (int day=0;day<7;++day) if (payload[1]&(1<<day)) {
        device->thermostat.ensure().schedules[day]=payload[2]==1 ? legacy.str() : normalized.str();
        device->thermostat.ensure().scheduleDetails[day]=normalized.str();
        device->thermostat.ensure().pending.erase(2*ThermostatField::Count+day);
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
        std::set<unsigned int>& target=attribute==65531 ? device->thermostat.ensure().attributes : device->thermostat.ensure().commands; target.clear();
        for(unsigned long i=0;i<values.size();++i) target.insert(static_cast<unsigned int>(strtoul(values[i].c_str(),0,10)));
        if(attribute==65531) device->thermostat.ensure().attributesKnown=true;
        else { device->thermostat.ensure().commandsKnown=true; device->thermostat.ensure().scheduleSupported=target.count(1) && target.count(2); }
        return true;
    }
    if(kind=="thermostat_schedule") {
        std::vector<unsigned char> bytes;
        JsonFields fields; std::vector<std::string> entries;
        if(!BoundedJson(value).parse(&fields) || !BoundedJson(fields["transitions"]).parseArray(&entries))return false;
        bool supported=device->thermostat.read().scheduleSupported; device->thermostat.ensure().scheduleSupported=true;
        if(!entries.empty() && !thermostatScheduleForDevice(*device,value,attribute-65536,&bytes)) { device->thermostat.ensure().scheduleSupported=supported;return false; }
        if(entries.empty()) {bytes.push_back(0);bytes.push_back(static_cast<unsigned char>(1<<(attribute-65536)));bytes.push_back(static_cast<unsigned char>(atoi(fields["mode"].c_str())));}
        return applyThermostatSchedule(device,bytes);
    }
    unsigned long count; const ThermostatAttribute* list=thermostatAttributes(&count);
    for(unsigned long i=0;i<count;++i) if(list[i].cluster==513 && list[i].attribute==attribute) {
        if(value=="null") { device->thermostat.ensure().values.erase(list[i].id); if(attribute==0){device->temperature.ensure().known=false;device->temperature.ensure().observation.at=0;}return true; }
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
        if(!thermostatSupported(device,*thermostatAttribute(ThermostatField::temperature_sensor_select)) ||
           (value!="builtin"&&!thermostatSupported(device,*thermostatAttribute(ThermostatField::external_temperature_input))) || value.empty()) {
            *error="external temperature is unavailable"; return false;
        }
        return true;
    }
    std::vector<unsigned char> payload;
    if(field.size()==10&&field.compare(0,9,"schedule_")==0&&field[9]>='0'&&field[9]<='6') {
        if(value=="__read__" ? (isTrvzb(device)||device.thermostat.read().scheduleSupported) :
           thermostatScheduleForDevice(device,value,field[9]-'0',&payload))return true;
        *error="invalid or unsupported thermostat schedule";return false;
    }
    if(!device.thermostatSourceId.empty()&&(field=="external_temperature_input"||field=="temperature_sensor_select")) {
        *error="switch to manual or built-in temperature source first";return false;
    }
    return thermostatEncode(device,field,value,&payload,error);
}

std::string thermostatJson(const Device& device) {
    std::ostringstream out; const ThermostatState& t=device.thermostat.read();
    out<<"{\"values\":{";
    bool first=true;
    for(unsigned i=0;i<ThermostatField::Count;++i) {
        ThermostatField::Id id=static_cast<ThermostatField::Id>(i);
        if(!t.values.count(id))continue;
        if(!first)out<<',';
        first=false;
        out<<'"'<<attributes[i].name<<"\":"<<t.values.get(id,0);
    }
    out<<"},\"supported\":{"; unsigned long count; thermostatAttributes(&count);
    for (unsigned long i=0;i<count;++i) { if(i)out<<','; out<<'"'<<attributes[i].name<<"\":"<<(thermostatSupported(device,attributes[i])?"true":"false"); }
    out<<"},\"fields\":{";
    for(unsigned long i=0;i<count;++i) { if(i)out<<','; ThermostatAttribute a=thermostatDefinition(device,attributes[i]);
        out<<'"'<<a.name<<"\":{\"min\":"<<a.minimum<<",\"max\":"<<a.maximum<<",\"step\":"<<a.step<<",\"writable\":"<<(a.writable?"true":"false")<<'}'; }
    out<<"},\"modes\":["; bool firstMode=true;
    for(int mode=0;mode<=9;++mode) if(thermostatModeSupported(device,mode)) { if(!firstMode)out<<','; firstMode=false;out<<mode; }
    out<<"],\"schedule_supported\":"<<((isTrvzb(device)||t.scheduleSupported)?"true":"false")<<",\"schedule_max_transitions\":"<<(isTrvzb(device)?6:static_cast<int>(thermostatValue(device,ThermostatField::number_of_daily_transitions,thermostatValue(device,ThermostatField::number_of_weekly_transitions,10))))
       <<",\"schedule_midnight_start\":"<<(isTrvzb(device)?"true":"false")<<",\"external_input_supported\":"<<(isTrvzb(device)?"true":"false")
       <<",\"schedule_details\":[";
    for(int day=0;day<7;++day) {if(day)out<<',';std::map<int,std::string>::const_iterator it=t.scheduleDetails.find(day);out<<(it==t.scheduleDetails.end()?"null":it->second);}
    out<<"],\"pending\":[";
    first=true;
    for(unsigned i=0;i<ThermostatPendingCount;++i)if(t.pending.count(i)) {
        if(!first)out<<',';
        first=false;
        out<<'"'<<jsonEscape(thermostatPendingName(i))<<'"';
    }
    out<<"],\"schedules\":[";
    for (int day=0;day<7;++day) {
        if(day)out<<',';
        std::map<int,std::string>::const_iterator it=t.schedules.find(day);
        if (it==t.schedules.end()) out<<"null"; else out<<'"'<<jsonEscape(it->second)<<'"';
    }
    out<<"],\"source_id\":\""<<jsonEscape(device.thermostatSourceId)<<"\",\"stale_minutes\":"<<device.thermostatStaleMinutes
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
        std::set<unsigned int>& target=attribute==65531?d->thermostat.ensure().attributes:d->thermostat.ensure().commands;
        target.clear();target.insert(v.list.value.items.begin(),v.list.value.items.end());
        if(attribute==65531)d->thermostat.ensure().attributesKnown=true;else{d->thermostat.ensure().commandsKnown=true;d->thermostat.ensure().scheduleSupported=target.count(1)&&target.count(2);}return true;
    }
    if(a.type.value=="thermostat_schedule"){
        const MatterProto::Measurement& m=v.measurement.value;const std::vector<MatterProto::ThermostatTransition>& entries=m.transitions.value.items;
        const unsigned maximum=isTrvzb(*d)?6:static_cast<unsigned>(thermostatValue(*d,ThermostatField::number_of_daily_transitions,thermostatValue(*d,ThermostatField::number_of_weekly_transitions,10)));
        if(entries.size()>maximum||(isTrvzb(*d)&&(m.mode.value!=1||(!entries.empty()&&entries[0].time.value!=0)))){ return false; }
        std::vector<unsigned char> bytes;bytes.push_back(entries.size());bytes.push_back(1U<<(attribute-65536));bytes.push_back(m.mode.value);
        for(size_t i=0;i<entries.size();++i){const MatterProto::ThermostatTransition& t=entries[i];put16(&bytes,t.time.value);
            for(unsigned bit=1;bit<=2;bit<<=1)if(m.mode.value&bit){const ThermostatAttribute* base=thermostatAttribute(bit==1?ThermostatField::occupied_heating_setpoint:ThermostatField::occupied_cooling_setpoint);const ThermostatAttribute def=thermostatDefinition(*d,*base);const double value=bit==1?t.heat.value:t.cool.value;
                if(!thermostatSupported(*d,*base)||value<def.minimum||value>def.maximum||std::fabs(value/def.step-std::floor(value/def.step+.5))>.00001){ return false; }
                put16(&bytes,static_cast<unsigned>(static_cast<long>(std::floor(value*100+.5))));
            }
        }d->thermostat.ensure().scheduleSupported=true;return applyThermostatSchedule(d,bytes);
    }
    unsigned long count;const ThermostatAttribute* list=thermostatAttributes(&count);
    for(unsigned long i=0;i<count;++i)if(list[i].cluster==513&&list[i].attribute==attribute){
        if(v.null_value.present){d->thermostat.ensure().values.erase(list[i].id);if(attribute==0){d->temperature.ensure().known=false;d->temperature.ensure().observation.at=0;}return true;}
        const uint64_t raw=v.integer.present?static_cast<uint64_t>(v.integer.value):v.unsigned_value.value;std::vector<unsigned char> bytes;
        for(unsigned b=0;b<width(list[i].type);++b)bytes.push_back(static_cast<unsigned char>(raw>>(8*b)));
        return applyThermostatAttribute(d,513,attribute,list[i].type,bytes,now);
    }{ return false; }
}
