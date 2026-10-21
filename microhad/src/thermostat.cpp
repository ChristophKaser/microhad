#include "thermostat.h"
#include "gateway.h"
#include "json_util.h"
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
    {"local_temperature", 0x0201, 0, 0x29, 100, -273.15, 327.67, 0.01, false, ""},
    {"occupied_heating_setpoint", 0x0201, 0x12, 0x29, 100, 4, 35, .5, true, ""},
    {"local_temperature_calibration", 0x0201, 0x10, 0x28, 10, -12.7, 12.7, .2, true, ""},
    {"system_mode", 0x0201, 0x1c, 0x30, 1, 0, 4, 1, true, ""},
    {"running_state", 0x0201, 0x29, 0x19, 1, 0, 65534, 1, false, ""},
    {"child_lock", 0xfc11, 0, 0x10, 1, 0, 1, 1, true, ""},
    {"open_window", 0xfc11, 0x6000, 0x10, 1, 0, 1, 1, true, ""},
    {"frost_protection_temperature", 0xfc11, 0x6002, 0x29, 100, 4, 35, .5, true, ""},
    {"idle_steps", 0xfc11, 0x6003, 0x21, 1, 0, 65534, 1, false, ""},
    {"closing_steps", 0xfc11, 0x6004, 0x21, 1, 0, 65534, 1, false, ""},
    {"valve_opening_limit_voltage", 0xfc11, 0x6005, 0x21, 1, 0, 65534, 1, false, ""},
    {"valve_closing_limit_voltage", 0xfc11, 0x6006, 0x21, 1, 0, 65534, 1, false, ""},
    {"valve_motor_running_voltage", 0xfc11, 0x6007, 0x21, 1, 0, 65534, 1, false, ""},
    {"valve_opening_degree", 0xfc11, 0x600b, 0x20, 1, 0, 100, 1, true, "1.1.4"},
    {"valve_closing_degree", 0xfc11, 0x600c, 0x20, 1, 0, 100, 1, true, "1.1.4"},
    {"external_temperature_input", 0xfc11, 0x600d, 0x29, 100, 0, 99.9, .1, true, ""},
    {"temperature_sensor_select", 0xfc11, 0x600e, 0x20, 1, 0, 2, 1, true, ""},
    {"temperature_accuracy", 0xfc11, 0x6011, 0x29, 100, -1, -.2, .2, true, "1.3.0"},
    {"temporary_mode_select", 0xfc11, 0x6014, 0x20, 1, 0, 1, 1, true, ""},
    {"temporary_mode_duration", 0xfc11, 0x6015, 0x23, 60, 0, 1440, 1, true, ""},
    {"timer_mode_target_temp", 0xfc11, 0x6016, 0x29, 100, 4, 35, .5, true, ""},
    {"smart_temperature_control", 0xfc11, 0x6017, 0x18, 1, 0, 2, 2, true, ""}
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
bool thermostatSupported(const Device& device, const ThermostatAttribute& attribute) {
    if (!isTrvzb(device) || device.thermostat.unsupported.count(attribute.name)) return false;
    return !attribute.minimumVersion[0] || device.thermostat.values.count(attribute.name) ||
        versionAtLeast(device.softwareBuildId, attribute.minimumVersion);
}
bool thermostatNumber(const std::string& text, double* value) {
    if (text.empty()) return false;
    char* end=0; *value=strtod(text.c_str(), &end);
    return end != text.c_str() && *end == 0 && *value == *value && *value >= -1e10 && *value <= 1e10;
}
bool thermostatEncode(const Device& device, const std::string& field, const std::string& text,
                      std::vector<unsigned char>* payload, std::string* error) {
    const ThermostatAttribute* a=thermostatAttribute(field); double number=0;
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
    if ((field=="system_mode" && number!=0 && number!=1 && number!=4) ||
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
    long raw=static_cast<long>(std::floor(number*a->scale+.5));
    payload->clear(); put16(payload,a->attribute); payload->push_back(a->type);
    for (unsigned int i=0;i<width(a->type);++i) payload->push_back(static_cast<unsigned char>(static_cast<unsigned long>(raw)>>(8*i)));
    return true;
}
bool applyThermostatAttribute(Device* device, unsigned short cluster, unsigned short attribute,
                             unsigned char type, const std::vector<unsigned char>& value, long now) {
    if (!isTrvzb(*device)) return false;
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
            (std::string(a.name)=="system_mode" && number!=0 && number!=1 && number!=4) ||
            (std::string(a.name)=="smart_temperature_control" && number!=0 && number!=2)) return false;
        device->thermostat.unsupported.erase(a.name);
        device->thermostat.values[a.name]=number;
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
bool applyThermostatSchedule(Device* device, const std::vector<unsigned char>& payload) {
    if (!isTrvzb(*device) || payload.size()<7 || payload[0]<1 || payload[0]>6 || payload[2]!=1 ||
        payload.size()!=3UL+payload[0]*4UL || payload[1]==0 || (payload[1]&0x80)) return false;
    std::ostringstream text;
    for (unsigned int i=0;i<payload[0];++i) {
        unsigned short at=get16(payload,3+i*4), temp=get16(payload,5+i*4);
        if (i) text<<' ';
        text<<std::setfill('0')<<std::setw(2)<<at/60<<':'<<std::setw(2)<<at%60<<'/'<<temp/100.0;
    }
    std::vector<unsigned char> checked;
    if (!thermostatSchedule(text.str(),0,&checked)) return false;
    for (int day=0;day<7;++day) if (payload[1]&(1<<day)) device->thermostat.schedules[day]=text.str();
    return true;
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
    out<<"},\"pending\":[";
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
