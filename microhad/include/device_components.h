#ifndef MICROHAD_DEVICE_COMPONENTS_H
#define MICROHAD_DEVICE_COMPONENTS_H
#include "clone_ptr.h"
#include "thermostat.h"
#include <string>
#include <vector>
struct EndpointDescriptor {
    unsigned char endpoint;
    unsigned short profileId,deviceId;
    unsigned char deviceVersion;
    std::vector<unsigned short> inputClusters,outputClusters;
    EndpointDescriptor();
};
struct TemperatureState { bool known;double celsius;TemperatureObservation observation;TemperatureState():known(false),celsius(0){} };
struct HumidityState { bool known;double percent;HumidityState():known(false),percent(0){} };
struct ContactState { bool known,open;long triggerUntil;ContactState():known(false),open(false),triggerUntil(0){} };
struct WaterLeakState { bool known,detected;WaterLeakState():known(false),detected(false){} };
struct AirQualityState { bool known;std::string value;AirQualityState():known(false){} };
struct Co2State { bool known;double ppm;std::string unit;Co2State():known(false),ppm(0){} };
struct Pm25State { bool known;double microgramsM3;std::string unit;Pm25State():known(false),microgramsM3(0){} };
struct OccupancyState { bool occupied;OccupancyState():occupied(false){} };
struct BatteryState { int percent;BatteryState():percent(0){} };
struct LightState { bool known,on;LightState():known(false),on(false){} };
struct BrightnessState { bool known;int percent;BrightnessState():known(false),percent(0){} };
struct RemoteState {};
struct ActorState { bool known,on;ActorState():known(false),on(false){} };
struct PowerState { bool known;double watts;PowerState():known(false),watts(0){} };
struct WifiSignalState { int percent;WifiSignalState():percent(0){} };
struct ZigbeeDeviceState {
    unsigned short networkAddress;
    int linkQuality,iasZoneId;
    bool initialInterviewComplete;
    std::string configuredSignature;
    std::vector<EndpointDescriptor> endpoints;
    std::vector<unsigned char> nodeDescriptor;
    ZigbeeDeviceState():networkAddress(0xffff),linkQuality(0),iasZoneId(-1),initialInterviewComplete(false){}
};
struct MatterDeviceState {
    std::string state,metadataState,node;
    int endpoint,rssiDbm;
    std::vector<unsigned int> servers,clients,deviceTypes;
    MatterDeviceState():endpoint(-1),rssiDbm(1){}
};
struct ExternalDeviceState { std::string externalId,component; };
#endif
