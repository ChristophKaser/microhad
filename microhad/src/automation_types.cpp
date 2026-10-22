#include "automation_types.h"
#include "gateway.h"
#include "json_util.h"
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <iomanip>

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
    bool value(Value* out,unsigned int depth){space();if(depth>32||++nodes>8192||at>=text.size())return false;char c=text[at];
        if(c=='"'){out->type=Value::String;return quoted(&out->string);}
        if(c=='{'||c=='['){++at;bool object=c=='{';*out=object?Value::object():Value::array();space();if(at<text.size()&&text[at]==(object?'}':']')){++at;return true;}do{std::string key;if(object&&(!quoted(&key)||out->fields.count(key)||!take(':')))return false;Value child;if(!value(&child,depth+1))return false;if(object)out->fields[key]=child;else out->items.push_back(child);space();if(at<text.size()&&text[at]==(object?'}':']')){++at;return true;}}while(take(','));return false;}
        if(text.compare(at,4,"null")==0){at+=4;*out=Value();return true;}if(text.compare(at,4,"true")==0){at+=4;*out=Value(true);return true;}if(text.compare(at,5,"false")==0){at+=5;*out=Value(false);return true;}
        size_t begin=at;if(c=='-')++at;if(at==text.size())return false;if(text[at]=='0')++at;else{if(text[at]<'1'||text[at]>'9')return false;while(at<text.size()&&text[at]>='0'&&text[at]<='9')++at;}if(at<text.size()&&text[at]=='.'){++at;size_t digits=at;while(at<text.size()&&text[at]>='0'&&text[at]<='9')++at;if(at==digits)return false;}if(at<text.size()&&(text[at]=='e'||text[at]=='E')){++at;if(at<text.size()&&(text[at]=='+'||text[at]=='-'))++at;size_t digits=at;while(at<text.size()&&text[at]>='0'&&text[at]<='9')++at;if(at==digits)return false;}double n=strtod(text.substr(begin,at-begin).c_str(),0);if(!std::isfinite(n))return false;*out=Value(n);return true;
    }
public: explicit Parser(const std::string& t):text(t),at(0),nodes(0){}bool run(Value* v){if(text.size()>262144||!value(v,0))return false;space();return at==text.size();}
};
}
bool parse(const std::string& text,Value* value){return Parser(text).run(value);}
Value snapshot(const Device& d){Value v=Value::object();
    const bool zigbee=d.source.empty()||d.source=="zigbee";
    v.fields["id"]=Value(d.ieeeAddress);
    v.fields["name"]=Value(d.name);v.fields["source"]=Value(d.source.empty()?"zigbee":d.source);v.fields["kind"]=Value(d.kind);v.fields["online"]=Value(d.online||(!zigbee&&d.source!="matter"&&d.lastSeen>0)||(zigbee&&d.lastSeen>0));v.fields["last_seen"]=Value(static_cast<double>(d.lastSeen));
#define FIELD(known,name,member) if(known)v.fields[name]=Value(d.member)
    FIELD(d.hasTemperature&&d.temperatureKnown,"temperature_celsius",temperatureCelsius);
    FIELD(d.hasHumidity&&(d.humidityKnown||zigbee),"humidity_percent",humidityPercent);
    FIELD(d.hasContact&&d.contactKnown,"contact_open",contactOpen);
    FIELD(d.hasWaterLeak&&d.waterLeakKnown,"water_detected",waterDetected);
    FIELD(d.hasOccupancy,"occupied",occupied);
    FIELD(d.hasLight&&(d.lightKnown||zigbee),"light_on",lightOn);
    FIELD(d.hasActor&&d.actorKnown,"actor_on",actorOn);
    FIELD(d.hasPower&&d.powerKnown,"power_watts",powerWatts);
    FIELD(d.hasAirQuality&&d.airQualityKnown,"air_quality",airQuality);
    FIELD(d.hasCo2&&d.co2Known,"co2_ppm",co2Ppm);
    FIELD(d.hasPm25&&d.pm25Known,"pm25_micrograms_m3",pm25MicrogramsM3);
#undef FIELD
    if(d.hasBrightness&&(d.brightnessKnown||zigbee))v.fields["brightness_percent"]=Value(static_cast<double>(d.brightnessPercent));
    if(d.hasBattery)v.fields["battery_percent"]=Value(static_cast<double>(d.batteryPercent));
    if(d.hasWifiSignal)v.fields["wifi_signal_percent"]=Value(static_cast<double>(d.wifiSignalPercent));
    if(d.source=="zigbee"||d.source.empty())v.fields["link_quality"]=Value(static_cast<double>(d.linkQuality));
    return v;
}
}
