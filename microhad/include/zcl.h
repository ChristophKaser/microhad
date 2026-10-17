#ifndef MICROHAD_ZIGBEE_ZCL_H
#define MICROHAD_ZIGBEE_ZCL_H

#include <string>
#include <vector>

struct Device;
enum { ZclObservedTemperature = 1, ZclObservedHumidity = 2, ZclObservedContact = 4,
       ZclObservedOccupancy = 8, ZclObservedBattery = 16, ZclObservedWaterLeak = 32 };

/* Applies standard, model-independent ZCL attributes. Returns true only when
   the attribute is understood and changed the normalized device state. */
bool applyZclAttribute(Device* device, unsigned short cluster, unsigned short attribute,
                       const std::vector<unsigned char>& value, long observedAt, unsigned* observed = 0);

/* IAS Zone status-change notification payload (command 0x00). */
bool applyIasZoneStatus(Device* device, const std::vector<unsigned char>& payload, long observedAt);

/* ZCL global Read Attributes Response records, beginning with attribute ID. */
bool applyZclReadAttributeResponse(Device* device, unsigned short cluster,
                                   const std::vector<unsigned char>& payload, long observedAt, unsigned* observed = 0);

/* Standard Home Automation On/Off and Level Control attributes. */
bool isLightAttribute(unsigned short cluster, unsigned short attribute);

std::vector<unsigned char> buildZclConfigureReporting(unsigned char transactionSequence, unsigned short attribute,
                                                       unsigned char dataType, unsigned short minimumInterval,
                                                       unsigned short maximumInterval, unsigned short reportableChange);

#endif
