#ifndef MICROHAD_NOTIFICATION_SOURCE_H
#define MICROHAD_NOTIFICATION_SOURCE_H
#include <string>

// Wire categories shared with the companion. Empty means an older caller.
inline bool validNotificationSource(const std::string& source) {
    return source == "default" || source == "window" || source == "doorbell" ||
        source == "temperature" || source == "humidity" || source == "leak" ||
        source == "battery" || source == "power" || source == "air_quality";
}
inline std::string notificationSourceFromKey(const std::string& key) {
    if (key.compare(0,8,"contact:")==0 && key.size()>15 && key.substr(key.size()-7)==":window") return "window";
    if (key.compare(0,6,"value:")==0 || key.compare(0,7,"legacy:")==0) {
        const size_t end=key.rfind(':');
        if(end!=std::string::npos && end>0) {
            const size_t start=key.rfind(':',end-1);
            if(start!=std::string::npos) {
                const std::string reading=key.substr(start+1,end-start-1);
                if(reading=="co2" || reading=="pm25")return "air_quality";
                if(reading=="temperature" || reading=="humidity" || reading=="battery" || reading=="power")return reading;
            }
        }
    }
    return "default";
}
inline std::string notificationChannel(const std::string& source) {
    return validNotificationSource(source) && source!="default" ? "microhad.alerts."+source : "microhad.gateway_alerts";
}
inline std::string notificationIcon(const std::string& source) {
    return validNotificationSource(source) && source!="default" ? "ic_notification_"+source : "ic_gateway";
}
#endif
