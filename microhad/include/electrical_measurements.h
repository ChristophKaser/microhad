#ifndef MICROHAD_ELECTRICAL_MEASUREMENTS_H
#define MICROHAD_ELECTRICAL_MEASUREMENTS_H
#include <map>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <stdint.h>
struct ElectricalDefinition { unsigned cluster,attribute,sub;const char* key;const char* unit;double divisor;unsigned char metric; };
inline const ElectricalDefinition* electricalDefinitions(unsigned* count) {
    static const ElectricalDefinition definitions[]={
#define ELECTRICAL(c,a,s,k,u,d,m) {c,a,s,#k,u,d,m},
#include "../../config/electrical-measurements.def"
#undef ELECTRICAL
    };
    *count=sizeof(definitions)/sizeof(definitions[0]);return definitions;
}
inline const ElectricalDefinition* electricalDefinition(const std::string& key) { unsigned count;const ElectricalDefinition* d=electricalDefinitions(&count);for(unsigned i=0;i<count;++i)if(key==d[i].key)return d+i;return 0; }
inline const ElectricalDefinition* electricalMetric(unsigned metric) { unsigned count;const ElectricalDefinition* d=electricalDefinitions(&count);for(unsigned i=0;i<count;++i)if(metric==d[i].metric)return d+i;return 0; }
inline bool electricalAttribute(unsigned cluster,unsigned attribute) { unsigned count;const ElectricalDefinition* d=electricalDefinitions(&count);for(unsigned i=0;i<count;++i)if(cluster==d[i].cluster&&attribute==d[i].attribute)return true;return cluster==145&&attribute==5; }
struct ElectricalMeasurement {
    bool known;double value;std::string unit;std::map<std::string, uint64_t> metadata;time_t observedAt;unsigned endpoint;
    ElectricalMeasurement():known(false),value(0),observedAt(0),endpoint(0){}
};
struct ElectricalState { std::map<std::string,ElectricalMeasurement> values;std::map<std::string, uint64_t> resetMetadata; };
inline void electricalMetadataJson(std::ostringstream& out,const std::map<std::string, uint64_t>& fields) {
    for(std::map<std::string, uint64_t>::const_iterator i=fields.begin();i!=fields.end();++i)out<<",\""<<i->first<<"\":"<<i->second;
}
inline std::string electricalJson(const ElectricalState& state) {
    std::ostringstream out;out<<std::setprecision(17)<<'{';bool first=true;
    for(std::map<std::string,ElectricalMeasurement>::const_iterator i=state.values.begin();i!=state.values.end();++i){
        if(!electricalDefinition(i->first))continue;
        if(!first)out<<',';
        first=false;const ElectricalMeasurement& v=i->second;
        out<<'"'<<i->first<<"\":{\"value\":";if(v.known)out<<v.value;else out<<"null";
        out<<",\"unit\":\""<<v.unit<<"\",\"quality\":\""<<(v.known?"measured":"unknown")<<"\",\"observed_at\":"<<v.observedAt<<",\"endpoint\":"<<v.endpoint;electricalMetadataJson(out,v.metadata);out<<'}';
    }
    if(!state.resetMetadata.empty()){if(!first)out<<',';out<<"\"cumulative_energy_reset\":{";bool comma=false;for(std::map<std::string,uint64_t>::const_iterator i=state.resetMetadata.begin();i!=state.resetMetadata.end();++i){if(comma)out<<',';comma=true;out<<'"'<<i->first<<"\":"<<i->second;}out<<'}';}
    out<<'}';return out.str();
}
#endif
