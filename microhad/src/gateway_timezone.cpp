#include "gateway_timezone.h"
#include <pthread.h>
#include <stddef.h>
extern "C" {
int32_t gateway_tz_find(const uint8_t*, size_t);
int32_t gateway_tz_offsets(uint32_t, int64_t, int32_t*, int32_t*);
const char* gateway_tz_name(unsigned);
}
namespace { pthread_mutex_t processorMutex=PTHREAD_MUTEX_INITIALIZER; }
bool gatewayTimeZoneValid(const std::string& name) {
    return gateway_tz_find(reinterpret_cast<const uint8_t*>(name.data()),name.size())>=0;
}
bool gatewayTimeZoneOffset(const std::string& name, int64_t unixSeconds, long* offset, long* standardOffset) {
    int32_t zone=gateway_tz_find(reinterpret_cast<const uint8_t*>(name.data()),name.size());
    if(zone<0)return false;
    int32_t standard=0,total=0;
    pthread_mutex_lock(&processorMutex);
    bool ok=gateway_tz_offsets(static_cast<uint32_t>(zone),unixSeconds,&standard,&total)==0;
    pthread_mutex_unlock(&processorMutex);
    if(ok) { *offset=total; if(standardOffset)*standardOffset=standard; }
    return ok;
}
std::string gatewayTimeZonesJson() {
    std::string json="[";
    for(unsigned i=0;gateway_tz_name(i);++i) {
        if(i)json+=',';
        json+='"';json+=gateway_tz_name(i);json+='"';
    }
    return json+"]";
}
