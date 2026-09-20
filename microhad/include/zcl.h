#ifndef MICROHAD_ZIGBEE_ZCL_H
#define MICROHAD_ZIGBEE_ZCL_H

#include <string>
#include <vector>

struct Device;

/* Applies standard, model-independent ZCL attributes. Returns true only when
   the attribute is understood and changed the normalized device state. */
bool applyZclAttribute(Device* device, unsigned short cluster, unsigned short attribute,
                       const std::vector<unsigned char>& value, long observedAt);

/* IAS Zone status-change notification payload (command 0x00). */
bool applyIasZoneStatus(Device* device, const std::vector<unsigned char>& payload, long observedAt);

/* ZCL global Read Attributes Response records, beginning with attribute ID. */
bool applyZclReadAttributeResponse(Device* device, unsigned short cluster,
                                   const std::vector<unsigned char>& payload, long observedAt);

#endif
